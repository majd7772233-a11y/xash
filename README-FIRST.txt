MAGD New - Complete Connection/Server Fix Package

Base: branch New of majd7772233-a11y/xash

Use the paths exactly as listed in Documentation/MAGD_CONNECTION_FIX_PLAN.md.

This package contains complete replacement files for the MAGD transport, TLS plumbing, Cloudflare Worker/Durable Objects backend, tests, and server CI.

It intentionally does NOT replace net_ws.c, host.c, net_http_xash.c, or engine/wscript because the current New branch already integrates those correctly for the MAGD public API.

Before production:
- provide a valid CA bundle for Xash TLS;
- set MAGD_JWT_SECRET in Cloudflare;
- run npm check/test and wrangler dry-run;
- deploy the Worker;
- perform a real two-device Internet multiplayer test.
