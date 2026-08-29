'use strict';

// A minimal static file server for the built wasm_decode_demo/ directory -
// no extra npm dependency for something Node's own http module already does.
// Used only as playwright.config.js's webServer command; not part of the
// demo itself (index.html says "any static file server", and this repo's
// own CMakeLists.txt comment names `python3 -m http.server` as the reference
// one - this is the same idea, kept in Node so the whole test toolchain is
// one runtime).

const http = require('http');
const fs = require('fs');
const path = require('path');

const root = path.resolve(process.argv[2] || '.');
const port = Number(process.argv[3] || 4173);

const mimeTypes = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.wasm': 'application/wasm',
    '.ec3': 'application/octet-stream',
    '.ac3': 'application/octet-stream',
    '.wav': 'audio/wav',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
};

const server = http.createServer((req, res) => {
    const requestPath = decodeURIComponent((req.url || '/').split('?')[0]);
    const resolved = path.join(root, requestPath === '/' ? '/index.html' : requestPath);
    // Refuse anything that escaped `root` via ../ - this only ever serves a
    // build output directory to a local test browser, but there is no reason
    // to trust the request path further than that.
    if (!resolved.startsWith(root)) {
        res.writeHead(403);
        res.end();
        return;
    }
    fs.readFile(resolved, (err, data) => {
        if (err) {
            res.writeHead(404);
            res.end();
            return;
        }
        res.writeHead(200, {
            'Content-Type': mimeTypes[path.extname(resolved)] || 'application/octet-stream',
            // Cross-origin isolation, required for SharedArrayBuffer - the
            // AudioWorklet pipeline's ring buffer (js/src/ring-buffer.ts)
            // needs it, and js/README.md documents this as a real deployment
            // requirement for anyone embedding the package, not just a test
            // fixture of this harness.
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        });
        res.end(data);
    });
});

server.listen(port, '127.0.0.1', () => {
    process.stdout.write(`serving ${root} on http://127.0.0.1:${port}\n`);
});
