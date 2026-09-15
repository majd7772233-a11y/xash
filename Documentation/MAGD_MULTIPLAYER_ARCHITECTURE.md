# MAGD Multiplayer Platform Architecture Specification
## Xash3D FWGS + MAGD Online Platform

---

### 1. Executive Summary

The objective of this project is to turn the **Xash3D FWGS** game engine repository into **Xash3D + MAGD Multiplayer Platform**, providing seamless online multiplayer functionality over the internet without modifying the original Half-Life / Xash3D game networking logic or breaking mod compatibility.

Key Design Goals:
1. **Preserve Original Game Code:** Keep Xash3D game DLLs, entity networking, prediction, and delta compression untouched.
2. **Host From Home:** Allow players to host games from home or mobile devices (Android / PC) without requiring Port Forwarding, Static IPs, or Router configuration.
3. **Control Plane on Cloudflare:** `xash-server` hosted as a Cloudflare Worker using Durable Objects for state management (Rooms, Server Discovery, Player Presence, Authentication).
4. **Transport Plane Abstraction:** Encapsulate raw Xash3D UDP datagrams into opaque binary payloads over a tunnel adapter (WebSocket binary relay initial prototype, evaluated against UDP relay solutions).
5. **Native In-Engine UI:** Add integrated MAGD Online UI menus directly inside the engine menu system (Create Room, Room Browser, Join by Code, Passwords, Player Limit).

---

### 2. High-Level Architecture Diagram

```
                              INTERNET
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │      xash-server      │
                     │   Cloudflare Worker   │
                     ├───────────────────────┤
                     │ API / Auth / Tokens   │
                     │ Rooms (Durable Obj)   │
                     │ Matchmaking / Discovery│
                     │ WebSocket Relay Hub   │
                     └───────────┬───────────┘
                                 │
                         MAGD Online Layer
                                 │
                 ┌───────────────┴───────────────┐
                 │                               │
        Xash3D Client                      Xash3D Server
        Android / PC                       Home / VPS / S9
                 │                               │
                 └────── Original Multiplayer ───┘
                           (UDP Datagrams)
```

---

### 3. Component Architecture

#### 3.1 Control Plane (`xash-server`)
- **Technology Stack:** Cloudflare Workers, Durable Objects, TypeScript, Wrangler.
- **REST / WebSocket endpoints:**
  - `GET /api/v1/health` - Health check.
  - `GET /api/v1/rooms` - List active rooms / servers.
  - `POST /api/v1/rooms/create` - Register a new host session and get Room Code (e.g. `MAGD-7F3A`).
  - `POST /api/v1/auth/guest` - Issue session token for guest players.
  - `WS /ws/room/:roomId` - Durable Object WebSocket endpoint for host & client tunnel connections.

#### 3.2 Engine Transport Abstraction (`engine/common/magd_net.*`)
- Hooks into `net_ws.c` / `net_chan.c` without replacing Xash3D's `NetChannel` or `UDP socket` model.
- Provides three transport modes:
  1. `MAGD_NET_MODE_LAN`: Standard LAN broadcasting and direct local sockets.
  2. `MAGD_NET_MODE_DIRECT_IP`: Direct IP connection (`connect <ip:port>`).
  3. `MAGD_NET_MODE_TUNNEL`: Routes Xash datagrams through `xash-server` room tunnel using binary frames.

#### 3.3 Host From Home Outbound Model
```
Host Engine Server (Local UDP 27015)
        │
        ▼ (Outbound WebSocket / Tunnel)
  xash-server (Cloudflare Durable Object)
        ▲ (Outbound WebSocket / Tunnel)
        │
Client Engine (Local UDP client socket)
```
- The host opens an **outbound** persistent connection to `xash-server`.
- No port forwarding is required on the host's router.
- `xash-server` routes client frames to the host's Durable Object session, which unwraps and feeds them into the local Xash server instance.

---

### 4. Binary Protocol Specification (MAGD Protocol v1)

Control and transport frames sent over the tunnel use a simple, low-overhead binary framing format:

```
+-------------------+-------------------+------------------------+
| Magic (2 bytes)   | Message Type (1B) | Payload Length (2B)    |
| 'M' 'G' (0x4D 47) | 0x01 - 0xFF       | Big-endian uint16      |
+-------------------+-------------------+------------------------+
| Payload Data (Variable Length)                                 |
+----------------------------------------------------------------+
```

#### Message Types:
- `0x01 HELLO`: Handshake initiation with token and version.
- `0x02 WELCOME`: Server acknowledgment with assigned Client ID.
- `0x03 HEARTBEAT / PING`: Keep-alive ping.
- `0x04 PONG`: Keep-alive response.
- `0x10 HOST_REGISTER`: Host initializing a room.
- `0x11 HOST_INFO_UPDATE`: Host updating player count, map name, etc.
- `0x20 JOIN_ROOM`: Client requesting to join a room.
- `0x30 GAME_DATAGRAM`: Raw Xash3D UDP packet wrapped inside binary frame.
- `0xE0 ERROR`: Protocol/Auth error.

---

### 5. In-Engine UI Integration Plan

Menus will be integrated into the native Xash3D engine UI (`engine/client/vgui` / menu implementation):
1. **MAGD ONLINE Menu item** on main menu.
2. **Create Room Screen:**
   - Server Name (e.g. "Majd's Server")
   - Game Mode / Mod Selection
   - Map Name (e.g. `crossfire`, `bounce`)
   - Max Players (1 - 32)
   - Password (Optional)
   - [ Start Server ]
3. **Server Browser Screen:**
   - List active rooms fetched from `xash-server`.
   - Displays Room Code, Host Name, Map, Players, Ping indicator.
   - [ Refresh ] | [ Join Room ] | [ Join by Code ]

---

### 6. Transport Evaluation Strategy

To ensure high-performance multiplayer gameplay, the project will benchmark transport mechanisms:

1. **Test 1 (Prototype):** UDP -> Binary WebSocket Tunnel (Durable Object Relay) -> UDP.
2. **Test 2 (Direct/TURN Relay):** UDP -> Dedicated UDP Relay Node -> UDP.
3. **Metrics Evaluated:**
   - Round-Trip Time (RTT / Ping)
   - Jitter & Packet loss rate
   - Head-of-line blocking behavior
   - Memory & CPU usage on Android/PC hosts

---

### 7. Phased Roadmap

- **Phase 0:** Architecture Documentation & Baseline (Completed)
- **Phase 1 & 2:** Codebase Audit & Baseline Networking Analysis
- **Phase 3:** Engine Network Abstraction Layer (`magd_net`)
- **Phase 4 & 5:** `xash-server` Foundation & MAGD Protocol Implementation
- **Phase 6 & 7:** Engine UI & Network Adapter Wiring
- **Phase 8 & 9:** Host From Home & MAGD Platform Features
- **Phase 10 & 11:** Transport Evaluation, Security, Verification & Release
