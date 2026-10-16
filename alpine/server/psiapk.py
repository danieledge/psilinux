"""psiapk.py - PsiProxy's side of Alpine mode's apk (linux/docs/ALPINE.md).

Alpine's catalogue (main and community, about 25,000 packages for armhf) is
far too big for a Psion's memory: Alpine's own apk needs about 30 MB to load
it, the 5mx has 9.5 MB. So this helper holds the real indexes (APKINDEX,
downloaded from Alpine's mirror and refreshed) and answers what apk would
work out from them: searches, package details, and which packages an
install needs. The Psion's `apk` (a wrapper) then downloads Alpine's own
signed .apk files and installs them with the real apk, from local files,
which checks their signatures against Alpine's keys as always. This module
never builds or changes a package.

All answers are text/plain, one item a line, fields separated by tabs, for
BusyBox's wget and shell on the Psion. Package names are [A-Za-z0-9_.+-].

    GET  /_apk/stats
        -> #PSIAPK1 <packages> <index time UTC> <branch> <arch>
    GET  /_apk/update
        -> the same, after fetching the indexes again (if they changed)
    GET  /_apk/search?q=PATTERN[&d=1][&n=MAX]
        -> name version repo size installed-size description   (sorted)
           PATTERN: a glob (* and ?) or a substring of the name; d=1 also
           matches descriptions. At most n lines (default 200).
    GET  /_apk/info?p=NAME
        -> field<TAB>value lines: name version description url license
           size installed-size depends provides origin repo file
    POST /_apk/resolve?add=NAME,NAME[&upgrade=1]
         body: the Psion's installed packages, "name version" a line
        -> #PSIAPK-RESOLVE <count> <download bytes> <installed bytes>
           then, for each package to fetch, in install order:
           name version repo file size installed-size checksum
           Lines starting "!" are problems (a name not found: "!missing NAME").
           With upgrade=1 and no add=, everything installed that has a newer
           version in the index (and what that needs).
    GET  /_apk/get/<repo>/<file>.apk
        -> the package itself, as Alpine's mirror has it (kept here too)

Settings (environment): PSIAPK_MIRROR (https://dl-cdn.alpinelinux.org/alpine),
PSIAPK_BRANCH (v3.24), PSIAPK_ARCH (armhf), PSIAPK_CACHE
(~/.cache/psiapk), PSIAPK_REFRESH (seconds between index checks, 21600).
"""

# PsiProxy loads this with PSIPROXY_PLUGINS=psiapk (no plug-in list needed)
PSIPROXY_ROUTE = "/_apk/"

import os
import re
import io
import time
import tarfile
import threading
import urllib.request

MIRROR = os.environ.get("PSIAPK_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")
BRANCH = os.environ.get("PSIAPK_BRANCH", "v3.24")
ARCH = os.environ.get("PSIAPK_ARCH", "armhf")
CACHE = os.environ.get("PSIAPK_CACHE", os.path.expanduser("~/.cache/psiapk"))
REFRESH = int(os.environ.get("PSIAPK_REFRESH", "21600"))
REPOS = ("main", "community")
NAME_RE = re.compile(r"^[A-Za-z0-9_.+-]{1,100}$")
FILE_RE = re.compile(r"^[A-Za-z0-9_.+-]{1,160}\.apk$")

_log = print
_lock = threading.Lock()
_db = None              # the parsed indexes (see _load)
_loaded = 0.0


def set_logger(fn):
    global _log
    _log = fn


# ---- Alpine's version order (apk's own rules, as far as Alpine uses them) ----

_SUFFIX = {"alpha": -4, "beta": -3, "pre": -2, "rc": -1, "cvs": 1, "svn": 2, "git": 3, "hg": 4, "p": 5}


def _vkey(v):
    """a sort key for an Alpine version such as 1.2.3a_rc1_p2-r4"""
    rel = 0
    m = re.match(r"^(.*)-r(\d+)$", v)
    if m:
        v, rel = m.group(1), int(m.group(2))
    main, _, suf = v.partition("_")
    parts = []
    for p in main.split("."):
        m = re.match(r"^(\d*)([a-z]?)$", p)
        if m:
            parts.append((int(m.group(1) or 0), m.group(2)))
        else:
            parts.append((0, p))
    sufs = []
    for s in suf.split("_") if suf else []:
        m = re.match(r"^([a-z]+)(\d*)$", s)
        if m:
            sufs.append((_SUFFIX.get(m.group(1), 0), int(m.group(2) or 0)))
    return (parts, sufs or [(0, 0)], rel)


def vcmp(a, b):
    ka, kb = _vkey(a), _vkey(b)
    return (ka > kb) - (ka < kb)


# ---- the indexes --------------------------------------------------------------

def _fetch_index(repo):
    """the repository's APKINDEX.tar.gz, from the mirror (kept in CACHE)"""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, "APKINDEX-%s-%s-%s.tar.gz" % (BRANCH, repo, ARCH))
    url = "%s/%s/%s/%s/APKINDEX.tar.gz" % (MIRROR, BRANCH, repo, ARCH)
    req = urllib.request.Request(url, headers={"User-Agent": "psiapk/1"})
    if os.path.exists(path):
        req.add_header("If-Modified-Since", time.strftime("%a, %d %b %Y %H:%M:%S GMT", time.gmtime(os.path.getmtime(path))))
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
        tmp = path + ".new"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except urllib.error.HTTPError as e:
        if e.code != 304:
            raise
    return path


def _parse(path, repo, db):
    with tarfile.open(path) as t:
        text = t.extractfile("APKINDEX").read().decode("utf-8", "replace")
    for block in text.split("\n\n"):
        f = {}
        for line in block.split("\n"):
            if len(line) > 2 and line[1] == ":":
                f.setdefault(line[0], []).append(line[2:])
        if "P" not in f or "V" not in f:
            continue
        name = f["P"][0]
        pk = {
            "name": name, "version": f["V"][0], "repo": repo,
            "size": int(f.get("S", ["0"])[0]), "isize": int(f.get("I", ["0"])[0]),
            "desc": f.get("T", [""])[0], "url": f.get("U", [""])[0], "license": f.get("L", [""])[0],
            "origin": f.get("o", [""])[0], "csum": f.get("C", [""])[0],
            "depends": f.get("D", [""])[0].split(), "provides": f.get("p", [""])[0].split(),
            "prio": int(f.get("k", ["0"])[0] or 0),
        }
        old = db["pkgs"].get(name)
        if old and vcmp(old["version"], pk["version"]) >= 0:
            continue
        db["pkgs"][name] = pk


def _provname(atom):
    """the name part of a dependency or provides atom: so:libz.so.1=1.3 -> so:libz.so.1"""
    return re.split(r"[<>=~]", atom, 1)[0]


def _load(force=False):
    global _db, _loaded
    with _lock:
        if _db is not None and not force and time.time() - _loaded < REFRESH:
            return _db
        db = {"pkgs": {}, "prov": {}, "time": 0}
        for repo in REPOS:
            path = _fetch_index(repo)
            _parse(path, repo, db)
            db["time"] = max(db["time"], int(os.path.getmtime(path)))
        for name, pk in db["pkgs"].items():
            db["prov"].setdefault(name, []).append(name)
            for p in pk["provides"]:
                db["prov"].setdefault(_provname(p), []).append(name)
        _db = db
        _loaded = time.time()
        _log("psiapk: %d packages (%s %s)" % (len(db["pkgs"]), BRANCH, ARCH))
        return db


# ---- answers ----------------------------------------------------------------------

def _file(pk):
    return "%s-%s.apk" % (pk["name"], pk["version"])


def stats(db):
    return "#PSIAPK1\t%d\t%s\t%s\t%s\n" % (len(db["pkgs"]), time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(db["time"])), BRANCH, ARCH)


def search(db, q, desc=False, limit=200):
    q = (q or "").strip()
    if any(c in q for c in "*?["):
        rx = re.compile("^" + re.escape(q).replace(r"\*", ".*").replace(r"\?", ".").replace(r"\[", "[").replace(r"\]", "]") + "$", re.I)
        test = lambda pk: rx.match(pk["name"]) or (desc and rx.search(pk["desc"]))
    else:
        low = q.lower()
        test = lambda pk: low in pk["name"].lower() or (desc and low in pk["desc"].lower())
    hits = [pk for pk in db["pkgs"].values() if test(pk)]
    # exact name first, then names that start with it, then the rest
    hits.sort(key=lambda pk: (pk["name"] != q, not pk["name"].startswith(q), pk["name"]))
    out = []
    for pk in hits[:limit]:
        out.append("%s\t%s\t%s\t%d\t%d\t%s" % (pk["name"], pk["version"], pk["repo"], pk["size"], pk["isize"], pk["desc"]))
    if len(hits) > limit:
        out.append("!more\t%d" % (len(hits) - limit))
    return "\n".join(out) + "\n"


def info(db, name):
    pk = db["pkgs"].get(name)
    if not pk:
        alt = db["prov"].get(name)
        pk = db["pkgs"].get(alt[0]) if alt else None
    if not pk:
        return "!missing\t%s\n" % name
    rows = [("name", pk["name"]), ("version", pk["version"]), ("description", pk["desc"]), ("url", pk["url"]),
            ("license", pk["license"]), ("size", str(pk["size"])), ("installed-size", str(pk["isize"])),
            ("depends", " ".join(pk["depends"])), ("provides", " ".join(pk["provides"])), ("origin", pk["origin"]),
            ("repo", pk["repo"]), ("file", _file(pk))]
    return "".join("%s\t%s\n" % r for r in rows)


def _provider(db, atom, have):
    """the package that satisfies a dependency atom: one installed already
    (by name or by what it provides), or the best in the index"""
    name = _provname(atom.lstrip("!"))
    cands = db["prov"].get(name, [])
    for c in cands:
        if c in have:
            return c, True
    if name in have:
        return name, True
    if not cands:
        return None, False
    # the package of that very name, else the highest provider priority
    if name in cands:
        return name, False
    cands = sorted(cands, key=lambda c: (-db["pkgs"][c]["prio"], c))
    return cands[0], False


def resolve(db, add, have, upgrade=False):
    """the packages to fetch for 'add' (names), given what is installed
    ('have': name -> version), in an order apk can install them in"""
    order, seen, problems = [], set(), []
    wanted = list(add)
    if upgrade:
        for n, v in have.items():
            pk = db["pkgs"].get(n)
            if pk and vcmp(pk["version"], v) > 0:
                wanted.append(n)

    def visit(name, depth):
        if name in seen or depth > 64:
            return
        seen.add(name)
        pk = db["pkgs"][name]
        for d in pk["depends"]:
            if d.startswith("!"):
                continue
            p, installed = _provider(db, d, have)
            if p is None:
                problems.append("!unsatisfied\t%s\t%s" % (name, d))
                continue
            if installed and not (upgrade and p in have and vcmp(db["pkgs"].get(p, {"version": "0"})["version"], have[p]) > 0):
                continue
            visit(p, depth + 1)
        order.append(name)

    for w in wanted:
        if not NAME_RE.match(w):
            problems.append("!bad\t%s" % w)
            continue
        p, installed = _provider(db, w, {} if upgrade or w not in have else have)
        if p is None:
            problems.append("!missing\t%s" % w)
            continue
        if p in have and not upgrade and _provname(w) == p:
            pk = db["pkgs"][p]
            if vcmp(pk["version"], have[p]) <= 0:
                continue                 # installed and up to date
        visit(p, 0)
    dl = sum(db["pkgs"][n]["size"] for n in order)
    inst = sum(db["pkgs"][n]["isize"] for n in order)
    lines = ["#PSIAPK-RESOLVE\t%d\t%d\t%d" % (len(order), dl, inst)]
    for n in order:
        pk = db["pkgs"][n]
        lines.append("%s\t%s\t%s\t%s\t%d\t%d\t%s" % (n, pk["version"], pk["repo"], _file(pk), pk["size"], pk["isize"], pk["csum"]))
    return "\n".join(lines + problems) + "\n"


def _package_path(db, repo, fname):
    """the .apk, fetched from the mirror into CACHE if need be (only files
    the index names)"""
    if repo not in REPOS or not FILE_RE.match(fname):
        return None
    names = {_file(pk) for pk in db["pkgs"].values() if pk["repo"] == repo}
    if fname not in names:
        return None
    d = os.path.join(CACHE, "packages", BRANCH, repo, ARCH)
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, fname)
    if not os.path.exists(path):
        url = "%s/%s/%s/%s/%s" % (MIRROR, BRANCH, repo, ARCH, fname)
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "psiapk/1"}), timeout=120) as r:
            data = r.read()
        tmp = path + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    return path


def _have(body):
    have = {}
    for line in (body or b"").decode("utf-8", "replace").splitlines():
        parts = line.split()
        if len(parts) >= 2 and NAME_RE.match(parts[0]):
            have[parts[0]] = parts[1]
    return have


def serve(conn, method, path, query, body, client_close, http_response):
    """psiproxy_core's hook for /_apk/ (as psidist.serve)"""
    from urllib.parse import parse_qs
    q = {k: v[-1] for k, v in parse_qs(query or "").items()}
    ctype = "text/plain; charset=utf-8"

    def send(status, reason, text, ct=ctype):
        data = text.encode("utf-8") if isinstance(text, str) else text
        conn.t.send(http_response(status, reason, data, ct, False, close=client_close))
        return not client_close

    try:
        if path == "/_apk/update":
            return send(200, "OK", stats(_load(force=True)))
        db = _load()
        if path == "/_apk/stats":
            return send(200, "OK", stats(db))
        if path == "/_apk/search":
            return send(200, "OK", search(db, q.get("q", ""), q.get("d") == "1", min(int(q.get("n", "200") or 200), 2000)))
        if path == "/_apk/info":
            return send(200, "OK", info(db, q.get("p", "")))
        if path == "/_apk/resolve":
            add = [a for a in q.get("add", "").split(",") if a]
            return send(200, "OK", resolve(db, add, _have(body), q.get("upgrade") == "1"))
        if path.startswith("/_apk/get/"):
            parts = path.split("/")
            if len(parts) == 5:
                p = _package_path(db, parts[3], parts[4])
                if p:
                    with open(p, "rb") as f:
                        return send(200, "OK", f.read(), "application/octet-stream")
            return send(404, "Not Found", "no such package\n")
    except Exception as e:
        _log("psiapk: %s %s: %s" % (method, path, e))
        return send(502, "Bad Gateway", "psiapk: %s\n" % e)
    return send(404, "Not Found", "no such thing\n")
