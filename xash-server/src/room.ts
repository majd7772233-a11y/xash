import { parseHeader, createMessage, MagdMessageType, MagdSessionState, MagdSessionStateValue } from './protocol';

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

interface PeerSession {
  id: string;
  isHost: boolean;
  token?: string;
  state: MagdSessionStateValue;
}

export class MAGDRoomObject {
  private state: DurableObjectState;
  private sessions: Map<WebSocket, PeerSession> = new Map();
  private meta: RoomMeta | null = null;

  constructor(state: DurableObjectState) {
    this.state = state;
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === '/init' && request.method === 'POST') {
      const data = await request.json() as any;
      this.meta = {
        code: data.code,
        name: data.name || 'MAGD Server',
        map: data.map || 'crossfire',
        game: data.game || 'valve',
        hostName: data.hostName || 'Host',
        players: 1,
        maxPlayers: data.maxPlayers || 16,
        hasPassword: !!data.hasPassword,
        password: data.password || undefined,
        createdAt: Date.now(),
        lastHeartbeat: Date.now()
      };
      return new Response(JSON.stringify(this.meta), {
        headers: { 'Content-Type': 'application/json' }
      });
    }

    if (url.pathname === '/info') {
      if (!this.meta) {
        return new Response(JSON.stringify({ error: 'Room not found' }), {
          status: 404,
          headers: { 'Content-Type': 'application/json' }
        });
      }
      const { password, ...safeMeta } = this.meta;
      return new Response(JSON.stringify(safeMeta), {
        headers: { 'Content-Type': 'application/json' }
      });
    }

    if (url.pathname === '/ws' || url.pathname.startsWith('/ws/')) {
      if (request.headers.get('Upgrade') !== 'websocket') {
        return new Response('Expected WebSocket', { status: 400 });
      }

      const token = url.searchParams.get('token');
      if (!token || !token.startsWith('magd_token_')) {
        return new Response('Unauthorized: Invalid or missing MAGD token', { status: 401 });
      }

      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);

      const clientId = crypto.randomUUID();
      const isHost = url.searchParams.get('role') === 'host';

      if (isHost && !this.meta) {
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
      this.sessions.set(server, { id: clientId, isHost, token, state: MagdSessionState.AUTHENTICATED });

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
        } else if (data.type === 'READY') {
          session.state = MagdSessionState.READY;
        }
      } catch {}
      return;
    }

    if (message.byteLength > 16384) {
      return;
    }

    const header = parseHeader(message);
    if (!header) return;

    if (header.type === MagdMessageType.GAME_DATAGRAM) {
      session.state = MagdSessionState.IN_ROOM;
      if (session.isHost) {
        for (const [peer, peerSession] of this.sessions.entries()) {
          if (!peerSession.isHost) {
            try { peer.send(message); } catch {}
          }
        }
      } else {
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
        for (const peer of this.sessions.keys()) {
          if (peer !== ws) {
            try { peer.close(1001, 'Host disconnected'); } catch {}
          }
        }
        this.sessions.clear();
        this.meta = null;
      } else if (this.meta && this.meta.players > 1) {
        this.meta.players--;
      }
    }
  }
}
