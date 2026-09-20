# MAGD Connection Fix — branch New

## Files in this package

Engine replacement:
- `engine/common/magd_net.c`
- `engine/common/magd_net.h`

Backend replacement/additions:
- `xash-server/src/index.ts`
- `xash-server/src/room.ts`
- `xash-server/src/auth.ts`
- `xash-server/src/protocol.ts`
- `xash-server/src/registry.ts`
- `xash-server/wrangler.jsonc`
- `xash-server/package.json`
- `xash-server/tsconfig.json`
- `xash-server/tests/*.ts`

## Transport path

Xash UDP packet -> MAGD envelope -> masked WebSocket binary frame -> TLS -> Cloudflare Worker -> Durable Object -> WebSocket -> Xash.

The engine uses the existing Xash mbedTLS HTTP/TLS layer already present in branch `New`; those TLS files do not need replacement for this package.

## Cloudflare secret

Before deployment:

```text
cd xash-server
npx wrangler secret put MAGD_JWT_SECRET
npx wrangler deploy
```

Do not hardcode the production secret.

## Important

The package is a repair implementation, not proof of completed online gameplay. It still needs compilation, deployment, and a real two-device test over different networks before being considered production-ready.
