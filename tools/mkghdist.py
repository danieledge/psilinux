#!/usr/bin/env python3
"""mkghdist.py - a #PSIDIST1 list of a GitHub repository's Psion programs.

    python3 tools/mkghdist.py [-o FILE]                      the Psion 2026 list (below)
    python3 tools/mkghdist.py --repo OWNER/REPO [--tag TAG] -o psidist.txt
                                                             one repository's release
    python3 tools/mkghdist.py --release-dir DIR --repo OWNER/REPO --tag TAG -o psidist.txt
                                                             the same, from files here
    python3 tools/mkghdist.py ... --check FILE               is FILE still right? (exit 1 if not)

The Psion's program installer reads a list in this format straight from
GitHub, over HTTPS, and downloads each build from the address it gives,
checking every file against the SHA-256 listed before installing it. Any
repository can publish one for its programs: attach the list to each
release as psidist.txt, and the stable address

    https://github.com/OWNER/REPO/releases/latest/download/psidist.txt

(which GitHub redirects to the newest release's copy) is the repository's
source: on the Psion, Tools > Sources... > Add GitHub source..., OWNER/REPO.
tools/psidist-release.yml is a GitHub Actions workflow that runs this on
each release and attaches the list.

The format, tab-separated:

    #PSIDIST1 <count>
    <name> <uid> <major> <minor> <size> <sha256> <mtime> <url>
    #NEEDS <name> <uid> <major> <minor> <requisite's name>   (its SIS requisites)
    #TEST <name>                                             (a test build: a prerelease,
                                                              or "test" in its name)
    #ABOUT <name> <title> <folder> <info> <subtitle>         (--about: what it is)
    #DESC <name> <description>

<name> is the file's name without ".sis": 1 to 32 letters, digits, '_', '-'
or '.' (not first or last, never two dots together). <uid> and the version
come from the file's own SIS header; <mtime> is seconds since 1970 UTC.

Sources:
  --repo OWNER/REPO       the .sis files attached to its latest release (or
                          --tag TAG's); a prerelease's are tagged #TEST
  --branch OWNER/REPO:BRANCH:FOLDER:FILE[,FILE...]
                          files in a branch, addressed by the commit
                          (raw.githubusercontent.com/OWNER/REPO/<sha>/...), so
                          an address always gives the bytes listed
  --release-dir DIR       with --repo and --tag: the release's files from DIR
                          (in a workflow, before or instead of downloading
                          them); addressed as the release's assets
With none of these, the Psion 2026 list (PRESET below): PsiTerm, PsiMail
and PsiWeb from danieledge/psiterm's dist/, and the latest releases of
danieledge/psicast and psiboost, public programs only (PsiLinux publishes its own).

  --allow REGEX           only names that match (the preset has its own)
  --about FILE            "<name><TAB><subtitle><TAB><description>" lines
                          (a name also covers its parts: PsiLinux covers
                          PsiLinux-root-2); the preset's are built in

Each file is downloaded (or read), its UID and version read from its SIS
header, its size and SHA-256 worked out. Anything whose name is not
allowed, whose header is not an EPOC SIS, or whose address is too long for
the Psion (URL_MAX) is left out, and said so on stderr.

Needs only python3 and the network (GitHub's API without a token: 60
requests an hour; GITHUB_TOKEN is used if set).
"""
import argparse
import calendar
import hashlib
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request

# the Psion 2026 list: its sources, and the names it may ever carry
PRESET_OWNER = "danieledge"
PRESET_BRANCH = [("psiterm", "main", "dist", ["PsiTerm.sis", "PsiMail.sis", "PsiWeb.sis"])]
# (PsiLinux is a source of its own: its release carries its own list)
PRESET_RELEASES = ["psicast", "psiboost"]
PRESET_ALLOW = r"^(PsiTerm|PsiMail|PsiWeb|PsiCast|PsiBoost|PsiLinux(-[A-Za-z0-9.-]+)?)$"
PRESET_ABOUT = {
    "PsiTerm": ("SSH terminal", "A secure terminal for the 5mx: SSH to any computer, with keys, sessions and "
                "file transfer."),
    "PsiMail": ("Email and calendar", "IMAP and SMTP email with pictures, and a CalDAV calendar, made for the "
                "5mx in 2026."),
    "PsiWeb": ("Web browser", "The Links 2 browser for the 5mx, with pictures."),
    "PsiCast": ("Screen sharing", "Shows the Psion's screen on a phone or computer, and lets it take control, "
                "through the PsiConnect Atom."),
    "PsiBoost": ("Link speed tests", "Tests and tunes the link between the Psion and the Atom."),
    "PsiLinux": ("Linux on the Psion", "Runs Linux on the 5mx, from the CF card."),
}

NAME_RE = re.compile(r"^(?!.*\.\.)[A-Za-z0-9_-](?:[A-Za-z0-9_.-]{0,30}[A-Za-z0-9_-])?$")
URL_MAX = 160 - 1                       # the longest file address the Psion keeps, with its 0
UA = "mkghdist (a #PSIDIST1 list of Psion programs)"
SIS_UID2 = 0x1000006D
SIS_UID3 = 0x10000419


# ---- EPOC R5 SIS files ------------------------------------------------------------

def sis_header(data):
    """(uid, major, minor) from the first 0x2C bytes of an EPOC R5 SIS file,
    or None: UID1 (the package) at 0, UID2 0x1000006D, UID3 0x10000419, the
    version's major and minor (16 bits each) at 0x28"""
    if len(data) < 0x2C:
        return None
    uid1, uid2, uid3 = struct.unpack_from("<III", data, 0)
    if uid2 != SIS_UID2 or uid3 != SIS_UID3 or uid1 == 0:
        return None
    major, minor = struct.unpack_from("<HH", data, 0x28)
    return uid1, major, minor


def requisites(data):
    """[(uid, major, minor, name)]: the programs it needs installed first (the
    languages' count at 0x12, the requisites' at 0x16, their records at the
    offset at 0x38: UID, major, minor, variant, then each language's name)"""
    head = data[:0x3C]
    if len(head) < 0x3C or not sis_header(head[:0x2C]):
        return []
    nlang, = struct.unpack_from("<H", head, 0x12)
    nreq, = struct.unpack_from("<H", head, 0x16)
    reqp, = struct.unpack_from("<I", head, 0x38)
    if nlang < 1 or nlang > 32 or reqp == 0:
        return []
    out, rec = [], 12 + 8 * nlang
    for i in range(min(nreq, 8)):
        r = data[reqp + i * rec:reqp + (i + 1) * rec]
        if len(r) < rec:
            break
        uid, major, minor = struct.unpack_from("<IHH", r, 0)
        nlen, = struct.unpack_from("<I", r, 12)
        nptr, = struct.unpack_from("<I", r, 12 + 4 * nlang)
        name = data[nptr:nptr + min(nlen, 64)] if 0 < nlen < 4096 else b""
        name = "".join(c for c in name.decode("latin-1") if 32 <= ord(c) < 127 and c != "\t")[:31].strip()
        if uid:
            out.append((uid, major, minor, name))
    return out


# ---- GitHub --------------------------------------------------------------------------

def get(url, accept=None):
    h = {"User-Agent": UA}
    if accept:
        h["Accept"] = accept
    tok = os.environ.get("GITHUB_TOKEN")
    if tok and url.startswith("https://api.github.com/"):
        h["Authorization"] = "Bearer " + tok
    with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=120) as r:
        return r.read()


def api(path):
    return json.loads(get("https://api.github.com" + path, "application/vnd.github+json"))


def when(iso):
    return calendar.timegm(time.strptime(iso, "%Y-%m-%dT%H:%M:%SZ"))


def release_candidates(repo, tag=None, local=None):
    """(name, url, mtime, size or None, data or None, test) for each .sis of a release"""
    rel = api("/repos/%s/releases/%s" % (repo, "tags/" + urllib.parse.quote(tag) if tag else "latest"))
    test = bool(rel.get("prerelease"))
    t = when(rel.get("published_at") or rel.get("created_at"))
    seen = set()
    for a in rel.get("assets", []):
        fn = a["name"]
        if fn.lower().endswith(".sis"):
            seen.add(fn)
            data = None
            if local and os.path.isfile(os.path.join(local, fn)):
                data = open(os.path.join(local, fn), "rb").read()
            yield fn[:-4], a["browser_download_url"], when(a["updated_at"]), a["size"], data, test
    if local:
        # (files not attached yet: they will be, under the release's address)
        for fn in sorted(os.listdir(local)):
            if fn.lower().endswith(".sis") and fn not in seen:
                url = "https://github.com/%s/releases/download/%s/%s" % (repo, urllib.parse.quote(rel["tag_name"]),
                                                                         urllib.parse.quote(fn))
                data = open(os.path.join(local, fn), "rb").read()
                yield fn[:-4], url, t, len(data), data, test


def branch_candidates(spec):
    """OWNER/REPO:BRANCH:FOLDER:FILE,FILE -> (name, url, mtime, None, None, False)"""
    repo, branch, folder, files = spec.split(":", 3)
    c = api("/repos/%s/commits/%s" % (repo, branch))
    sha, t = c["sha"], when(c["commit"]["committer"]["date"])
    for fn in files.split(","):
        path = "/".join(p for p in (folder.strip("/"), fn) if p)
        yield fn[:-4], "https://raw.githubusercontent.com/%s/%s/%s" % (repo, sha, path), t, None, None, False


def build(cands, allow):
    items, seen = [], set()
    for name, url, mtime, size, data, test in cands:
        if not NAME_RE.match(name) or (allow and not allow.match(name)):
            print("left out: %s (not a name this list takes)" % name, file=sys.stderr)
            continue
        if name.lower() in seen:
            print("left out: %s (listed already)" % name, file=sys.stderr)
            continue
        if len(url) > URL_MAX or not url.startswith("https://"):
            print("left out: %s (its address is too long for the Psion)" % name, file=sys.stderr)
            continue
        if data is None:
            data = get(url)
        if size is not None and len(data) != size:
            print("left out: %s (%d bytes, GitHub says %d)" % (name, len(data), size), file=sys.stderr)
            continue
        info = sis_header(data[:0x2C])
        if not info:
            print("left out: %s (not an EPOC SIS file)" % name, file=sys.stderr)
            continue
        seen.add(name.lower())
        items.append({"name": name, "uid": info[0], "major": info[1], "minor": info[2], "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest(), "mtime": mtime, "url": url,
                      "needs": requisites(data), "test": test or "test" in name.lower()})
        print("%-24s %08x %d.%02d %8d %s%s" % (name, info[0], info[1], info[2], len(data), url,
                                                " (test)" if items[-1]["test"] else ""), file=sys.stderr)
    items.sort(key=lambda e: e["name"].lower())
    return items


def clean(s, n):
    s = "".join(c if 32 <= ord(c) < 127 else " " for c in str(s or "")).strip()
    return s[:n]


def text(items, about=None):
    about = about or {}
    lines = ["#PSIDIST1\t%d" % len(items),
             "# Psion programs: name, package UID, version, size, SHA-256, time, address; "
             "#NEEDS: what a build needs installed first; #TEST: a test build. Generated: do not edit."]
    for e in items:
        lines.append("%s\t%08x\t%d\t%d\t%d\t%s\t%d\t%s" % (e["name"], e["uid"], e["major"], e["minor"],
                                                          e["size"], e["sha256"], e["mtime"], e["url"]))
        lines += ["#NEEDS\t%s\t%08x\t%d\t%d\t%s" % (e["name"], uid, major, minor, rname or "-")
                  for uid, major, minor, rname in e["needs"]]
        if e["test"]:
            lines.append("#TEST\t%s" % e["name"])
        a = about.get(e["name"]) or about.get(e["name"].split("-")[0])
        if a:
            base = e["name"].split("-")[0]
            lines.append("#ABOUT\t%s\t%s\t%s\t-\t%s" % (e["name"], e["name"], base, clean(a[0], 60) or "-"))
            if a[1]:
                lines.append("#DESC\t%s\t%s" % (e["name"], clean(a[1], 360)))
    return "\n".join(lines) + "\n"


def read_about(path):
    out = {}
    for line in open(path, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 2 and f[0] and not f[0].startswith("#"):
            out[f[0]] = (f[1], f[2] if len(f) > 2 else "")
    return out


def main():
    ap = argparse.ArgumentParser(description="a #PSIDIST1 list of a GitHub repository's Psion programs")
    ap.add_argument("-o", "--out", help="write the list here (else stdout)")
    ap.add_argument("--repo", action="append", default=[], help="OWNER/REPO: its latest release's .sis files")
    ap.add_argument("--tag", help="with one --repo: that release, not the latest")
    ap.add_argument("--branch", action="append", default=[], help="OWNER/REPO:BRANCH:FOLDER:FILE,FILE")
    ap.add_argument("--release-dir", help="with --repo and --tag: the release's files, here")
    ap.add_argument("--allow", help="only names matching this regular expression")
    ap.add_argument("--about", help="a file of <name> TAB <subtitle> TAB <description> lines")
    ap.add_argument("--check", metavar="FILE", help="exit 1 if FILE is not what would be written now")
    a = ap.parse_args()
    if (a.tag or a.release_dir) and len(a.repo) != 1:
        ap.error("--tag and --release-dir go with exactly one --repo")
    if a.repo or a.branch:
        cands = [c for b in a.branch for c in branch_candidates(b)]
        cands += [c for r in a.repo for c in release_candidates(r, a.tag, a.release_dir)]
        allow = re.compile(a.allow) if a.allow else None
        about = read_about(a.about) if a.about else {}
    else:
        cands = [c for repo, branch, folder, files in PRESET_BRANCH
                 for c in branch_candidates("%s/%s:%s:%s:%s" % (PRESET_OWNER, repo, branch, folder, ",".join(files)))]
        cands += [c for r in PRESET_RELEASES for c in release_candidates("%s/%s" % (PRESET_OWNER, r))]
        allow = re.compile(a.allow or PRESET_ALLOW)
        about = read_about(a.about) if a.about else PRESET_ABOUT
    t = text(build(cands, allow), about)
    if a.check:
        old = open(a.check).read() if os.path.exists(a.check) else ""
        same = old == t
        print("%s is %s" % (a.check, "up to date" if same else "out of date"), file=sys.stderr)
        sys.exit(0 if same else 1)
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, "w", newline="\n") as f:
            f.write(t)
    else:
        sys.stdout.write(t)


if __name__ == "__main__":
    main()
