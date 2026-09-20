# MAGD Online Connection Fix — based on branch New

## Files to replace/add

### Replace
- `engine/common/magd_net.c`
- `engine/common/magd_net.h`
- `engine/common/http/net_http_tls.c`
- `engine/common/http/net_http_tls.h`
- `xash-server/src/auth.ts`
- `xash-server/src/index.ts`
- `xash-server/src/protocol.ts`
- `xash-server/src/registry.ts`
- `xash-server/src/room.ts`
- `xash-server/wrangler.jsonc`
- `xash-server/package.json`
- `xash-server/tsconfig.json`
- `xash-server/tests/*.test.ts`

### Add
- `xash-server/.dev.vars.example`
- `.github/workflows/magd-server.yml`

## Files intentionally kept unchanged

- `engine/common/net_ws.c`
- `engine/common/net_ws_private.h`
- `engine/common/host.c`
- `engine/common/http/net_http_xash.c`
- `engine/wscript`

The existing `net_ws.c` already routes `NET_GetPacket()` and `NET_SendPacketEx()` through the MAGD public API when tunnel mode is active.

## Transport contract

Xash UDP -> MAGD envelope -> masked WebSocket binary frame -> TLS -> Cloudflare Worker -> Durable Object -> WebSocket -> MAGD envelope -> Xash UDP.

Client virtual host address is deterministic: `10.254.0.1:27015`.
Client peer addresses are `10.254.0.2:27015` through `10.254.0.32:27015`.

## Cloudflare deployment

1. Install dependencies inside `xash-server`:

```text
npm install
npm run check
npm test
npx wrangler deploy --dry-run
```

2. Store the production secret:

```text
npx wrangler secret put MAGD_JWT_SECRET
```

3. Deploy:

```text
npx wrangler deploy
```

4. Confirm:

```text
GET /api/v1/health
```

The current Wrangler configuration uses declarative Durable Object `exports` with SQLite storage.

## Important TLS requirement

Secure WSS now fails closed when no CA bundle is available, unless the privileged debug cvar `http_tls_insecure` is deliberately enabled.

Place a valid PEM CA bundle at the path configured by `http_tls_cafile` (default: `cacert.pem`) in a filesystem location visible to Xash. Do not ship production builds with `http_tls_insecure 1`.

## Real multiplayer test

After deployment:

1. Host device: `magd_create_room`.
2. Start the Xash game/server normally on the host.
3. Client device: `magd_connect MAGD-XXXX [password]`.
4. The client receives WELCOME and, when `magd_auto_connect 1`, issues `connect 10.254.0.1:27015`.
5. Test host -> client, client -> host, broadcast, targeted packets, client disconnect, Wi-Fi change, and host reconnect within the 30 second grace window.

This package is designed to make the codebase deployable and testable; a real two-device test is still required before declaring online gameplay production-ready.
