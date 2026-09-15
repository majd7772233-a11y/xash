# MAGD Multiplayer Platform Development & Build Guide

## 1. Building Xash3D Engine with MAGD Layer

### Linux / PC (64-bit Dedicated & Client)
```bash
./waf configure -d --64bits
./waf build
```

### Android Build
```bash
./waf configure --android=arm64-v8a,4.9,21
./waf build
```

---

## 2. Testing & Deploying `xash-server` (Cloudflare Backend)

### Local Unit & Performance Tests
```bash
cd xash-server
npm install
npm test
```

### Local Worker Development Server
```bash
cd xash-server
npx wrangler dev
```

### Production Cloudflare Worker Deployment
```bash
cd xash-server
npx wrangler deploy
```

---

## 3. Engine Commands Reference

- `magd_create_room` - Initializes an outbound room session and generates a MAGD room code.
- `magd_connect <code` - Joins an active MAGD room session by room code (e.g. `MAGD-7F3A`).
- `magd_room_info <code>` - Queries live room metadata from `xash-server`.
