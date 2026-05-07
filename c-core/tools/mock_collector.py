#!/usr/bin/env python3
import argparse
import gzip
import json
from http.server import BaseHTTPRequestHandler, HTTPServer


class CollectorHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/v1/batch":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.headers.get("Content-Encoding", "").lower() == "gzip":
            try:
                body = gzip.decompress(body)
            except OSError:
                self.send_response(400)
                self.end_headers()
                return

        try:
            payload = json.loads(body.decode("utf-8"))
        except json.JSONDecodeError:
            self.send_response(400)
            self.end_headers()
            return

        if not isinstance(payload.get("token"), str) or not isinstance(payload.get("batch"), list):
            self.send_response(400)
            self.end_headers()
            return

        print(json.dumps({"accepted": len(payload["batch"])}, sort_keys=True), flush=True)
        self.send_response(202)
        self.end_headers()
        self.wfile.write(b'{"ok":true}\n')

    def log_message(self, format, *args):
        return


def main():
    parser = argparse.ArgumentParser(description="Local Honch mock collector")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    server = HTTPServer((args.host, args.port), CollectorHandler)
    print(f"Honch mock collector listening on http://{args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
