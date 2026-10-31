#!/usr/bin/env python3
"""A stand-in for PsiProxy in the tests: an HTTP proxy that takes
"GET http://..." and "GET https://..." (it does the TLS itself, as PsiProxy
does) and logs each request line. CONNECT is refused, so a test sees which
way Links asked.
    proxy.py PORT LOG"""
import http.server, sys, urllib.request

class P(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def do_GET(self):
        with open(sys.argv[2], "a") as f:
            f.write(self.requestline + "\n")
        try:
            req = urllib.request.Request(self.path, headers={"User-Agent": self.headers.get("User-Agent", "")})
            with urllib.request.urlopen(req, timeout=20) as r:
                body = r.read()
                self.send_response(r.status)
                self.send_header("Content-Type", r.headers.get("Content-Type", "text/html"))
        except Exception as e:
            body = ("<html><body><h3>proxy: %s</h3></body></html>" % e).encode()
            self.send_response(502)
            self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
    def do_CONNECT(self):
        with open(sys.argv[2], "a") as f:
            f.write(self.requestline + "\n")
        self.send_error(405, "no CONNECT here")

http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), P).serve_forever()
