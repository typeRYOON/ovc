# python serve.py  →  http://localhost:8080/demo.html
# (python -m http.server mislabels .mjs/.wasm MIME on Windows; modules then refuse to load)
import http.server

class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".mjs": "text/javascript",
        ".js": "text/javascript",
        ".wasm": "application/wasm",
    }

http.server.ThreadingHTTPServer(("127.0.0.1", 8080), Handler).serve_forever()
