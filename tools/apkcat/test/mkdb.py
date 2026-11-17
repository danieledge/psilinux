#!/usr/bin/env python3
"""mkdb.py - installed-package databases for the wrapper tests, as apk's
/lib/apk/db/installed has them (P:, V:, p: only):
  db-image  the PsiLinux image's own (if it has been built)
  db-small  a small system: the closure of busybox, apk-tools and musl-utils
  db-old    db-small (or the image's) with some versions made older
            (and one package that has left the index), for upgrade

    python3 mkdb.py INDEXDIR IMAGEDB OUTDIR
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
idx, imgdb, out = sys.argv[1:4]
spec = importlib.util.spec_from_file_location("psiapk", os.path.join(HERE, "../../../alpine/server/psiapk.py"))
psiapk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(psiapk)
db = {"pkgs": {}, "prov": {}}
for repo in psiapk.REPOS:
    psiapk._parse(os.path.join(idx, "APKINDEX-%s.tar.gz" % repo), repo, db)
for name, pk in db["pkgs"].items():
    db["prov"].setdefault(name, []).append(name)
    for p in pk["provides"]:
        db["prov"].setdefault(psiapk._provname(p), []).append(name)


def write(path, entries):
    with open(path, "w") as f:
        for n, v, prov in entries:
            f.write("C:Q1xxxx=\nP:%s\nV:%s\nA:armhf\n" % (n, v))
            if prov:
                f.write("p:%s\n" % " ".join(prov))
            f.write("\n")


def read(path):
    ents, p, v, pr = [], None, None, []
    for line in open(path).read().split("\n") + [""]:
        if line.startswith("P:"):
            p = line[2:]
        elif line.startswith("V:"):
            v = line[2:]
        elif line.startswith("p:"):
            pr = line[2:].split()
        elif line == "" and p:
            ents.append((p, v, pr)); p, v, pr = None, None, []
    return ents


plan = psiapk.resolve(db, ["busybox", "apk-tools", "musl-utils", "alpine-baselayout"], {})
small = []
for l in plan.splitlines()[1:]:
    n = l.split("\t")[0]
    pk = db["pkgs"][n]
    small.append((n, pk["version"], pk["provides"]))
write(os.path.join(out, "db-small"), small)
base = small
if os.path.exists(imgdb):
    img = read(imgdb)
    write(os.path.join(out, "db-image"), img)
    base = img
old = []
for i, (n, v, pr) in enumerate(base):
    if i % 3 == 0:
        v = "0.0.1-r0"
    old.append((n, v, pr))
old.append(("gone-from-the-index", "1.0-r0", []))
write(os.path.join(out, "db-old"), old)
print("  databases: small %d packages%s, old %d" % (len(small), ", image %d" % len(base) if base is not small else "", len(old)))
