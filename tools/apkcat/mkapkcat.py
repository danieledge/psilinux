#!/usr/bin/env python3
"""mkapkcat.py - Alpine's package catalogue as small static files, for the
Psion's apk (alpine/overlay/usr/local/sbin/apk) with no PsiProxy.

Alpine's own apk needs about 30 MB of memory to load the full catalogue
(main and community, about 25,000 armhf packages); a 5mx has about 9 MB. So
this turns Alpine's APKINDEX into files of a few KB that the Psion reads one
at a time over its serial link, only the ones it needs. GitHub Pages serves
them (.github/workflows/apkcat.yml). The packages themselves still come
from Alpine's mirror and are checked by the real apk against Alpine's keys,
so these files need no trust beyond what they point to.

    python3 tools/apkcat/mkapkcat.py OUT [--index DIR] [--branch v3.24] [--arch armhf]

The indexes are downloaded from the mirror (APKCAT_MIRROR, default
https://dl-cdn.alpinelinux.org/alpine) unless --index names a folder with
APKINDEX-<repo>.tar.gz files (as psiapk's cache has, or the tests'). It
writes OUT/apk/<branch>/<arch>/ (format 1):

  stats       #PSIAPKCAT1 <packages> <index time UTC> <branch> <arch> <format>
              <index id> <built UTC> <provides buckets> <version shards>
              (one line, tabs; the index id is a hash of the two APKINDEXes)
  (every file is at most 8 KB)
  p/NAME      a package: "key<TAB>value" lines: name version description url
              license size installed-size depends origin repo file checksum
              (not what it provides: the Psion reads that from its own
              database once a package is installed), then "dep<TAB>ATOM<TAB>PROVIDER" for each dependency
              (the package that satisfies it when nothing installed does:
              the one of that name, else the highest provider_priority,
              then the first by name; "!" when nothing in the index does)
              A record over 8 KB is p/NAME.gz, and p/NAME is "gz<TAB>1".
  pv/N        what provides a name that is not a package (N = hash % 512):
              "name<TAB>package" lines, best provider first
  v/N.gz      every package's version: "name<TAB>version<TAB>repo"
              (N = hash % the number of shards in stats), for upgrade and list
  s/HEX.gz    the name search: packages whose name (lower case) holds the
              three characters HEX (their bytes in hex):
              "name<TAB>version<TAB>description": the name that is those
              three, then names starting with them, then the rest by name
              (at most 800 lines, and what fits: "!more<TAB>N" after)
  d/HEX.gz    the description search: packages with a word (a-z0-9, lower
              case) in their description that starts with those three
              characters, as s/ (at most 600 lines, and what fits)
  common      the trigrams in more than 150 names: "tri<TAB>count"
              (the Psion searches the rarest of its query's trigrams)

hash(s) is h = (h * 31 + byte) % 65536 over the bytes of s: the Psion's awk
does the same. Gzip files have no time stamp, so an unchanged file comes out
byte for byte the same.
"""

import argparse
import gzip
import hashlib
import io
import os
import re
import shutil
import sys
import tarfile
import time
import urllib.request
import email.utils

FORMAT = 1
REPOS = ("main", "community")
PV_BUCKETS = 512
D_MAX = 600
S_MAX = 800
# every file at most this big: the Psion reads them over a serial link, and
# BusyBox wget's https under PsiLinux 0.36 can damage what comes after the
# first 8 KB of an answer (apk checks each file it reads, too)
MAXF = 8192
COMMON_MIN = 150
MIRROR = os.environ.get("APKCAT_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")


# ---- Alpine's version order (as psiapk.py's, which follows apk's rules) ----

_SUFFIX = {"alpha": -4, "beta": -3, "pre": -2, "rc": -1, "cvs": 1, "svn": 2, "git": 3, "hg": 4, "p": 5}


def _vkey(v):
    rel = 0
    m = re.match(r"^(.*)-r(\d+)$", v)
    if m:
        v, rel = m.group(1), int(m.group(2))
    main, _, suf = v.partition("_")
    parts = []
    for p in main.split("."):
        m = re.match(r"^(\d*)([a-z]?)$", p)
        parts.append((int(m.group(1) or 0), m.group(2)) if m else (0, p))
    sufs = []
    for s in suf.split("_") if suf else []:
        m = re.match(r"^([a-z]+)(\d*)$", s)
        if m:
            sufs.append((_SUFFIX.get(m.group(1), 0), int(m.group(2) or 0)))
    return (parts, sufs or [(0, 0)], rel)


def vcmp(a, b):
    ka, kb = _vkey(a), _vkey(b)
    return (ka > kb) - (ka < kb)


def bucket(s, n):
    h = 0
    for c in s.encode("utf-8"):
        h = (h * 31 + c) % 65536
    return h % n


def provname(atom):
    return re.split(r"[<>=~]", atom, 1)[0]


# ---- the indexes ---------------------------------------------------------------

def fetch(branch, arch, repo, dest):
    url = "%s/%s/%s/%s/APKINDEX.tar.gz" % (MIRROR, branch, repo, arch)
    req = urllib.request.Request(url, headers={"User-Agent": "psilinux-apkcat/1"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
        lm = r.headers.get("Last-Modified")
    with open(dest, "wb") as f:
        f.write(data)
    if lm:
        t = email.utils.parsedate_to_datetime(lm).timestamp()
        os.utime(dest, (t, t))
    return dest


def parse(path, repo, pkgs):
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
            "size": int(f.get("S", ["0"])[0] or 0), "isize": int(f.get("I", ["0"])[0] or 0),
            "desc": f.get("T", [""])[0], "url": f.get("U", [""])[0], "license": f.get("L", [""])[0],
            "origin": f.get("o", [""])[0], "csum": f.get("C", [""])[0],
            "depends": f.get("D", [""])[0].split(), "provides": f.get("p", [""])[0].split(),
            "prio": int(f.get("k", ["0"])[0] or 0),
        }
        old = pkgs.get(name)
        if old and vcmp(old["version"], pk["version"]) >= 0:
            continue
        pkgs[name] = pk


def clean(s):
    return s.replace("\t", " ").replace("\r", " ").replace("\n", " ")


# ---- the files ----------------------------------------------------------------

class Writer:
    def __init__(self, root):
        self.root = root
        self.count = 0
        self.bytes = 0
        self.biggest = {}
        self.over = []

    def put(self, rel, text, gz=False):
        data = text.encode("utf-8")
        if gz:
            data = gzip.compress(data, 9, mtime=0)
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        if len(data) > MAXF:
            self.over.append(rel)
        self.count += 1
        self.bytes += len(data)
        kind = rel.split("/")[0]
        if len(data) > self.biggest.get(kind, (0, ""))[0]:
            self.biggest[kind] = (len(data), rel)


def fit(lines, total):
    """as many of the lines as fit in MAXF packed, and "!more N" for the rest"""
    def text(k):
        return "".join(lines[:k]) + ("!more\t%d\n" % (total - k) if k < total else "")
    lo, hi = 0, len(lines)
    if len(gzip.compress(text(hi).encode("utf-8"), 9, mtime=0)) <= MAXF:
        return text(hi)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if len(gzip.compress(text(mid).encode("utf-8"), 9, mtime=0)) <= MAXF:
            lo = mid
        else:
            hi = mid - 1
    return text(lo)


def build(pkgs, out, branch, arch, index_time, index_id):
    w = Writer(out)
    prov = {}
    for name, pk in pkgs.items():
        prov.setdefault(name, []).append(name)
        for p in pk["provides"]:
            prov.setdefault(provname(p), []).append(name)

    def best(atom):
        n = provname(atom.lstrip("!"))
        cands = prov.get(n, [])
        if not cands:
            return None
        if n in cands:
            return n
        return sorted(set(cands), key=lambda c: (-pkgs[c]["prio"], c))[0]

    # p/NAME
    for name, pk in pkgs.items():
        rows = [("name", name), ("version", pk["version"]), ("description", pk["desc"]), ("url", pk["url"]),
                ("license", pk["license"]), ("size", pk["size"]), ("installed-size", pk["isize"]),
                ("depends", " ".join(pk["depends"])), ("origin", pk["origin"]), ("repo", pk["repo"]), ("file", "%s-%s.apk" % (name, pk["version"])),
                ("checksum", pk["csum"])]
        text = "".join("%s\t%s\n" % (k, clean(str(v))) for k, v in rows)
        for d in pk["depends"]:
            if d.startswith("!"):
                continue
            text += "dep\t%s\t%s\n" % (d, best(d) or "!")
        if len(text.encode("utf-8")) > MAXF:
            # (a long list of dependencies: the record packed, and a note)
            w.put("p/%s.gz" % name, text, gz=True)
            w.put("p/" + name, "gz\t1\n")
        else:
            w.put("p/" + name, text)

    # pv/N: provided names that are not packages
    pv = {}
    for n, cands in prov.items():
        if n in pkgs:
            continue
        order = sorted(set(cands), key=lambda c: (-pkgs[c]["prio"], c))
        pv.setdefault(bucket(n, PV_BUCKETS), []).extend("%s\t%s\n" % (n, c) for c in order)
    for b in range(PV_BUCKETS):
        # (by name; a name's providers stay best first)
        w.put("pv/%d" % b, "".join(sorted(pv.get(b, []), key=lambda l: l.split("\t", 1)[0])))

    # v/N.gz: as few shards as keep each one under MAXF
    for nv in (16, 24, 32, 48, 64, 96, 128, 192, 256):
        vs = {}
        for name in sorted(pkgs):
            pk = pkgs[name]
            vs.setdefault(bucket(name, nv), []).append("%s\t%s\t%s\n" % (name, pk["version"], pk["repo"]))
        packed = [gzip.compress("".join(vs.get(b, [])).encode("utf-8"), 9, mtime=0) for b in range(nv)]
        if max(len(x) for x in packed) <= MAXF:
            break
    for b in range(nv):
        w.put("v/%d.gz" % b, "".join(vs.get(b, [])), gz=True)

    # s/HEX.gz and common
    tri = {}
    for name in pkgs:
        low = name.lower()
        for t in {low[i:i + 3] for i in range(len(low) - 2)}:
            tri.setdefault(t, []).append(name)
    common = []
    for t, names in tri.items():
        # the name that is the trigram, then names starting with it, then
        # the rest (as psiapk sorts a search for it); at most S_MAX
        names.sort(key=lambda n: (n.lower() != t, not n.lower().startswith(t), n))
        lines = ["%s\t%s\t%s\n" % (n, pkgs[n]["version"], clean(pkgs[n]["desc"])) for n in names[:S_MAX]]
        w.put("s/%s.gz" % t.encode().hex(), fit(lines, len(names)), gz=True)
        if len(names) > COMMON_MIN:
            common.append((t, len(names)))
    w.put("common", "".join("%s\t%d\n" % c for c in sorted(common)))

    # d/HEX.gz: words in descriptions
    words = {}
    for name, pk in pkgs.items():
        for pre in {wd[:3] for wd in re.findall(r"[a-z0-9]+", pk["desc"].lower()) if len(wd) >= 3}:
            words.setdefault(pre, []).append(name)
    for pre, names in words.items():
        names.sort()
        lines = ["%s\t%s\t%s\n" % (n, pkgs[n]["version"], clean(pkgs[n]["desc"])) for n in names[:D_MAX]]
        w.put("d/%s.gz" % pre.encode().hex(), fit(lines, len(names)), gz=True)

    built = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    w.put("stats", "#PSIAPKCAT1\t%d\t%s\t%s\t%s\t%d\t%s\t%s\t%d\t%d\n" % (
        len(pkgs), time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(index_time)), branch, arch,
        FORMAT, index_id, built, PV_BUCKETS, nv))
    return w


def index_id(paths):
    h = hashlib.sha256()
    for p in paths:
        with tarfile.open(p) as t:
            h.update(t.extractfile("APKINDEX").read())
    return h.hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", help="the site's folder (apk/<branch>/<arch>/ is written in it)")
    ap.add_argument("--index", help="a folder with APKINDEX-<repo>.tar.gz (else downloaded)")
    ap.add_argument("--branch", default="v3.24")
    ap.add_argument("--arch", default="armhf")
    ap.add_argument("--id-only", action="store_true", help="only print the index id (for the workflow)")
    a = ap.parse_args()

    paths = []
    tmp = None
    for repo in REPOS:
        if a.index:
            p = os.path.join(a.index, "APKINDEX-%s.tar.gz" % repo)
            if not os.path.exists(p):    # (psiapk's cache's names)
                p = os.path.join(a.index, "APKINDEX-%s-%s-%s.tar.gz" % (a.branch, repo, a.arch))
        else:
            tmp = tmp or os.path.join(a.out, ".apkindex")
            os.makedirs(tmp, exist_ok=True)
            p = fetch(a.branch, a.arch, repo, os.path.join(tmp, "APKINDEX-%s.tar.gz" % repo))
        paths.append(p)
    iid = index_id(paths)
    if a.id_only:
        print(iid)
        return
    pkgs = {}
    for repo, p in zip(REPOS, paths):
        parse(p, repo, pkgs)
    itime = max(int(os.path.getmtime(p)) for p in paths)
    root = os.path.join(a.out, "apk", a.branch, a.arch)
    if os.path.isdir(root):
        shutil.rmtree(root)
    w = build(pkgs, root, a.branch, a.arch, itime, iid)
    if tmp:
        shutil.rmtree(tmp)
    print("apkcat: %d packages, %d files, %.1f MB, index %s" % (len(pkgs), w.count, w.bytes / 1048576, iid))
    for k, (n, rel) in sorted(w.biggest.items()):
        print("  biggest %-6s %6d bytes  %s" % (k, n, rel))
    if w.over:
        sys.exit("apkcat: %d files over %d bytes: %s" % (len(w.over), MAXF, " ".join(w.over[:10])))


if __name__ == "__main__":
    main()
