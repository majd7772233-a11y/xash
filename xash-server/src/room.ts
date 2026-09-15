import { parseHeader, createMessage, MagdMessageType } from './protocol';

export interface RoomMeta {
  code: string;
  name: string;
  map: string;
  game: string;
  hostName: string;
  players: number;
  maxPlayers: number;
  hasPassword: boolean;
  password?: string;
  createdAt: number;
  lastHeartbeat: number;
}

export class MAGDRoomObject {
  private state: DurableObjectState;
  private sessions: Map<WebSocket, { id: string; isHost: boolean }> = new Map();
  private meta: RoomMeta | null = null;

  constructor(state: DurableObjectState) {
    this.state = state;
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === '/info') {
      if (!this.meta) {
        return new Response(JSON.stringify({ error: 'Room not found' }), {
          status: 404,
          headers: { 'Content-Type': 'application/json' }
        });
      }
      return new Response(JSON.stringify(this.meta), {
        headers: { 'Content-Type': 'application/json' }
      });
    }

    if (url.pathname === '/ws') {
      if (request.headers.get('Upgrade') !== 'websocket') {
        return new Response('Expected WebSocket', { status: 400 });
      }

      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);

      const clientId = crypto.randomUUID();
      const isHost = url.searchParams.get('role') === 'host';

      if (isHost) {
        const roomCode = url.searchParams.get('code') || 'MAGD-' + Math.random().toString(36).substring(2, 6).toUpperCase();
        this.meta = {
          code: roomCode,
          name: url.searchParams.get('name') || 'MAGD Server',
          map: url.searchParams.get('map') || 'crossfire',
          game: url.searchParams.get('game') || 'valve',
          hostName: url.searchParams.get('host') || 'Host',
          players: 1,
          maxPlayers: parseInt(url.searchParams.get('maxPlayers') || '16', 10),
          hasPassword: !!url.searchParams.get('password'),
          password: url.searchParams.get('password') || undefined,
          createdAt: Date.now(),
          lastHeartbeat: Date.now()
        };
      }

      this.state.acceptWebSocket(server);
      this.sessions.set(server, { id: clientId, isHost });

      // Send WELCOME
      const welcome = createMessage(MagdMessageType.WELCOME, new TextEncoder().encode(JSON.stringify({ clientId, isHost, code: this.meta?.code })));
      server.send(welcome);

      return new Response(null, { status: 101, webSocket: client });
    }

    return new Response('Not found', { status: 404 });
  }

  async webSocketMessage(ws: WebSocket, message: ArrayBuffer | string) {
    const session = this.sessions.get(ws);
    if (!session) return;

    if (typeof message === 'string') {
      try {
        const data = JSON.parse(message);
        if (data.type === 'HEARTBEAT' && this.meta) {
          this.meta.lastHeartbeat = Date.now();
          if (data.players !== undefined) this.meta.players = data.players;
          if (data.map !== undefined) this.meta.map = data.map;
          ws.send(JSON.stringify({ type: 'PONG', timestamp: Date.now() }));
        }
      } catch {}
      return;
    }

    // Binary packet relaying
    const header = parseHeader(message);
    if (!header) return;

    if (header.type === MagdMessageType.GAME_DATAGRAM) {
      if (session.isHost) {
        // Relay host datagram to all clients
        for (const [peer, peerSession] of this.sessions.entries()) {
          if (!peerSession.isHost) {
            try { peer.send(message); } catch {}
          }
        }
      } else {
        // Relay client datagram to host
        for (const [peer, peerSession] of this.sessions.entries()) {
          if (peerSession.isHost) {
            try { peer.send(message); } catch {}
          }
        }
      }
    }
  }

  async webSocketClose(ws: WebSocket) {
    const session = this.sessions.get(ws);
    if (session) {
      this.sessions.delete(ws);
      if (session.isHost) {
        // Host disconnected -> close all client sessions
        for (const peer of this.sessions.keys()) {
          try { peer.close(1001, 'Host disconnected'); } catch {}
        }
        this.sessions.clear();
        this.meta = null;
      } else if (this.meta && this.meta.players > 1) {
        this.meta.players--;
      }
    }
  }
}
