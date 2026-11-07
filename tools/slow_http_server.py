"""HTTP file server with added latency and a bandwidth cap, for testing
MxfSource over a slow link (http:// source, range requests).

Serves every file in ROOT at http://HOST:PORT/<name>. Supports the requests
the plugin sends: GET with "Range: bytes=a-b" or "bytes=a-" (206), plain GET
(200), HEAD. Each request waits --latency ms before the response starts
(think round trip plus server time to first byte); all transfers together
share --mbps (a global cap, like one network link). Clients that hang up
mid-transfer (the plugin aborts a stream when it seeks) are normal.

Example:
  python tools/slow_http_server.py D:/test --port 8080 --latency 80 --mbps 200
  MxfSource("http://127.0.0.1:8080/file.mxf", logfile="C:/temp/mxf.log")
"""
import argparse
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

CHUNK = 64 * 1024


class Link:
    """Global bandwidth cap: every chunk books a time slot on one shared line."""

    def __init__(self, mbps):
        self.rate = mbps * 1_000_000 / 8 if mbps > 0 else 0
        self.next_free = time.monotonic()
        self.lock = threading.Lock()
        self.sent = 0

    def wait_for(self, nbytes):
        with self.lock:
            self.sent += nbytes
            if not self.rate:
                return
            now = time.monotonic()
            start = max(now, self.next_free)
            self.next_free = start + nbytes / self.rate
        delay = start - time.monotonic()
        if delay > 0:
            time.sleep(delay)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    root = "."
    latency = 0.0
    link = None
    verbose = True
    count = 0
    count_lock = threading.Lock()

    def log_message(self, fmt, *args):
        if self.verbose:
            sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def _resolve(self):
        name = unquote(self.path.split("?", 1)[0]).lstrip("/")
        path = os.path.realpath(os.path.join(self.root, name))
        if not path.startswith(os.path.realpath(self.root)) or not os.path.isfile(path):
            return None
        return path

    def _headers(self, send_body):
        with Handler.count_lock:
            Handler.count += 1
        time.sleep(self.latency)
        path = self._resolve()
        if path is None:
            self.send_error(404)
            return None
        size = os.path.getsize(path)
        start, end, status = 0, size - 1, 200
        rng = self.headers.get("Range")
        if rng:
            m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
            if not m or (not m.group(1) and not m.group(2)):
                self.send_error(416)
                return None
            if m.group(1):
                start = int(m.group(1))
                end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
            else:  # suffix range: last N bytes
                start, end = max(0, size - int(m.group(2))), size - 1
            if start >= size or start > end:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            status = 206
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        return (path, start, end) if send_body else None

    def do_HEAD(self):
        self._headers(send_body=False)

    def do_GET(self):
        target = self._headers(send_body=True)
        if not target:
            return
        path, start, end = target
        remaining = end - start + 1
        try:
            with open(path, "rb") as f:
                f.seek(start)
                while remaining > 0:
                    data = f.read(min(CHUNK, remaining))
                    if not data:
                        break
                    self.link.wait_for(len(data))
                    self.wfile.write(data)
                    remaining -= len(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            self.close_connection = True  # client aborted the stream (seek): normal


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="directory to serve")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--latency", type=float, default=50, help="ms before each response starts (default 50)")
    ap.add_argument("--mbps", type=float, default=100, help="total bandwidth cap in Mbit/s, 0 = none (default 100)")
    ap.add_argument("--quiet", action="store_true", help="don't log every request")
    args = ap.parse_args()

    Handler.root = args.root
    Handler.latency = args.latency / 1000
    Handler.link = Link(args.mbps)
    Handler.verbose = not args.quiet
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True

    def stats():
        last = 0
        while True:
            time.sleep(10)
            sent = Handler.link.sent
            print("[stats] %d request(s), %.1f MB sent, %.1f Mbit/s over the last 10 s"
                  % (Handler.count, sent / 1e6, (sent - last) * 8 / 10e6), file=sys.stderr, flush=True)
            last = sent

    threading.Thread(target=stats, daemon=True).start()
    print("serving %s at http://%s:%d/ -- latency %g ms, cap %s"
          % (os.path.abspath(args.root), args.host, args.port, args.latency,
             "%g Mbit/s" % args.mbps if args.mbps > 0 else "none"), file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
