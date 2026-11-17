#!/usr/bin/env python3
"""psiapkd.py - psiapk.py's answers over plain HTTP, from local indexes, for
the tests (the real one runs inside PsiProxy): the reference the online
catalogue's resolver is compared with.

    python3 psiapkd.py PORT INDEXDIR [PSIAPK.PY]
"""
import http.server
import importlib.util
import os
import sys
from urllib.parse import urlsplit, parse_qs

port, idx = int(sys.argv[1]), sys.argv[2]
src = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "../../../alpine/server/psiapk.py")
spec = importlib.util.spec_from_file_location("psiapk", src)
psiapk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(psiapk)


def load():
    db = {"pkgs": {}, "prov": {}, "time": 0}
    for repo in psiapk.REPOS:
        p = os.path.join(idx, "APKINDEX-%s.tar.gz" % repo)
        if not os.path.exists(p):
            p = os.path.join(idx, "APKINDEX-%s-%s-%s.tar.gz" % (psiapk.BRANCH, repo, psiapk.ARCH))
        psiapk._parse(p, repo, db)
        db["time"] = max(db["time"], int(os.path.getmtime(p)))
    for name, pk in db["pkgs"].items():
        db["prov"].setdefault(name, []).append(name)
        for pr in pk["provides"]:
            db["prov"].setdefault(psiapk._provname(pr), []).append(name)
    return db


DB = load()


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def answer(self, body):
        u = urlsplit(self.path)
        q = {k: v[-1] for k, v in parse_qs(u.query).items()}
        p = u.path
        if p in ("/_apk/stats", "/_apk/update"):
            t = psiapk.stats(DB)
        elif p == "/_apk/search":
            t = psiapk.search(DB, q.get("q", ""), q.get("d") == "1", min(int(q.get("n", "200")), 2000))
        elif p == "/_apk/info":
            t = psiapk.info(DB, q.get("p", ""))
        elif p == "/_apk/resolve":
            add = [a for a in q.get("add", "").split(",") if a]
            t = psiapk.resolve(DB, add, psiapk._have(body), q.get("upgrade") == "1")
        else:
            self.send_response(404); self.end_headers(); return
        data = t.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.answer(b"")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        self.answer(self.rfile.read(n))


http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
