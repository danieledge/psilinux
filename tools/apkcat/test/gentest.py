#!/usr/bin/env python3
"""gentest.py - checks what mkapkcat.py made against the index it was made
from, with psiapk.py's own rules (its provider choice is the reference).
Also writes version pairs for the wrapper's version order test (vpairs).

    python3 gentest.py INDEXDIR CATDIR
"""
import gzip
import importlib.util
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
idx, cat = sys.argv[1], sys.argv[2]


def mod(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


psiapk = mod("psiapk", os.path.join(HERE, "../../../alpine/server/psiapk.py"))
mk = mod("mkapkcat", os.path.join(HERE, "../mkapkcat.py"))
db = {"pkgs": {}, "prov": {}}
for repo in psiapk.REPOS:
    psiapk._parse(os.path.join(idx, "APKINDEX-%s.tar.gz" % repo), repo, db)
for name, pk in db["pkgs"].items():
    db["prov"].setdefault(name, []).append(name)
    for p in pk["provides"]:
        db["prov"].setdefault(psiapk._provname(p), []).append(name)
pkgs = db["pkgs"]
errors = []


def err(s):
    if len(errors) < 30:
        print("  " + s)
    errors.append(s)


st = open(os.path.join(cat, "stats")).read().rstrip("\n").split("\t")
if st[0] != "#PSIAPKCAT1" or int(st[1]) != len(pkgs) or st[3] != "v3.24" or st[4] != "armhf" or st[5] != "1":
    err("stats: %r" % st)
npv, nv = int(st[8]), int(st[9])

# p/: every package, every field, every dependency's provider
for name, pk in pkgs.items():
    try:
        lines = open(os.path.join(cat, "p", name), encoding="utf-8").read().splitlines()
        if lines == ["gz\t1"]:
            lines = gzip.open(os.path.join(cat, "p", name + ".gz"), "rt", encoding="utf-8").read().splitlines()
    except OSError:
        err("no p/" + name)
        continue
    f, deps = {}, []
    for l in lines:
        k, _, v = l.partition("\t")
        if k == "dep":
            deps.append(tuple(v.split("\t")))
        else:
            f[k] = v
    want = {"name": name, "version": pk["version"], "repo": pk["repo"], "size": str(pk["size"]),
            "installed-size": str(pk["isize"]), "checksum": pk["csum"], "file": psiapk._file(pk),
            "depends": " ".join(pk["depends"])}
    for k, v in want.items():
        if f.get(k) != v:
            err("p/%s %s: %r, not %r" % (name, k, f.get(k), v))
    real = [d for d in pk["depends"] if not d.startswith("!")]
    if [d for d, _ in deps] != real:
        err("p/%s: deps %r" % (name, deps))
    for d, p in deps:
        ref, _ = psiapk._provider(db, d, {})
        if (ref or "!") != p:
            err("p/%s dep %s: %s, psiapk says %s" % (name, d, p, ref))

# pv/: the best provider of each provided name that is not a package
pv = {}
for b in range(npv):
    for l in open(os.path.join(cat, "pv", str(b))).read().splitlines():
        n, p = l.split("\t")
        if mk.bucket(n, npv) != b:
            err("pv/%d has %s" % (b, n))
        pv.setdefault(n, p)
for n in db["prov"]:
    if n in pkgs:
        continue
    ref, _ = psiapk._provider(db, n, {})
    if pv.get(n) != ref:
        err("pv %s: %s, psiapk says %s" % (n, pv.get(n), ref))

# v/: every package once, in its shard
seen = {}
for b in range(nv):
    for l in gzip.open(os.path.join(cat, "v", "%d.gz" % b), "rt").read().splitlines():
        n, v, r = l.split("\t")
        if mk.bucket(n, nv) != b:
            err("v/%d has %s" % (b, n))
        seen[n] = (v, r)
if seen != {n: (pk["version"], pk["repo"]) for n, pk in pkgs.items()}:
    err("v/: not every package, or not its version")

# s/: each name in the shard of each of its trigrams (unless that one was cut)
shard = {}


def s_names(t):
    if t not in shard:
        lines = gzip.open(os.path.join(cat, "s", t.encode().hex() + ".gz"), "rt").read().splitlines()
        shard[t] = ({l.split("\t")[0] for l in lines if not l.startswith("!")}, any(l.startswith("!more") for l in lines))
    return shard[t]


rnd = random.Random(1)
for name in rnd.sample(sorted(pkgs), 3000):
    low = name.lower()
    for i in range(len(low) - 2):
        names, cut = s_names(low[i:i + 3])
        if name not in names and not cut:
            err("s/%s: no %s" % (low[i:i + 3], name))

# sizes: a few KB
big = []
for root, _, files in os.walk(cat):
    for fn in files:
        sz = os.path.getsize(os.path.join(root, fn))
        big.append((sz, os.path.relpath(os.path.join(root, fn), cat)))
big.sort()
n8 = sum(1 for s, _ in big if s > 8192)
n4 = sum(1 for s, _ in big if s > 4096)
print("  %d files; %d over 4 KB (%.2f%%), none may be over 8 KB: the biggest: %s" % (len(big), n4, 100.0 * n4 / len(big),
      ", ".join("%s %d" % (r, s) for s, r in big[-3:])))
if n8:
    err("%d files over 8 KB" % n8)

# the wrapper's version order test: pairs and psiapk's answer
vs = sorted({pk["version"] for pk in pkgs.values()})
pairs = [("1.0", "1.0"), ("1.0", "1.0.1"), ("1.2_rc1", "1.2"), ("1.2", "1.2_p1"), ("1.2a", "1.2b"), ("1.2", "1.2a"),
         ("1.10", "1.9"), ("2.0-r1", "2.0-r10"), ("1.0_alpha2", "1.0_beta1"), ("1.0_git20240101", "1.0"),
         ("0.9_pre1-r2", "0.9-r0"), ("2026.1", "2026.01"), ("1.2.3_rc1_p2-r4", "1.2.3_rc1_p3-r0")]
for _ in range(3000):
    pairs.append((rnd.choice(vs), rnd.choice(vs)))
# (and versions of one package: the cases that matter)
byorigin = {}
for pk in pkgs.values():
    byorigin.setdefault(pk["version"].split(".")[0], []).append(pk["version"])
for g in list(byorigin.values())[:400]:
    if len(g) > 1:
        pairs.append((rnd.choice(g), rnd.choice(g)))
with open(os.path.join(os.path.dirname(cat.rstrip("/")), "..", "..", "..", "vpairs"), "w") as f:
    for a, b in pairs:
        f.write("%s %s %d\n" % (a, b, psiapk.vcmp(a, b)))
print("  %d packages, %d provided names; %d version pairs for the wrapper" % (len(pkgs), len(pv), len(pairs)))
if errors:
    print("  %d problems" % len(errors))
    sys.exit(1)
