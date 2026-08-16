#!/usr/bin/env python3
"""Serve the repo root so /editor/ can fetch config/ and boards/."""
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import os
import sys

ROOT = Path(__file__).resolve().parent.parent
os.chdir(ROOT)


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {
        **SimpleHTTPRequestHandler.extensions_map,
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".keymap": "text/plain",
        ".dtsi": "text/plain",
    }


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"Totem editor: http://127.0.0.1:{port}/editor/")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
