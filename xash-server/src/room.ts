import {
  createGameDatagram,
  createMessage,
  GAME_BROADCAST,
  GAME_HAS_TARGET,
  GAME_HAS_SENDER,
  MagdMessageType,
  parseGameDatagram,
  parseHeader,
} from './protocol.ts';
import { hashPassword } from './auth.ts';

const ROOM_META_KEY = 'meta';
const MAX_PACKET_SIZE = 16_384;
const MAX_ROOM_NAME = 64;
const MAX_MAP_NAME = 64;
const MAX_GAME_NAME = 32;
const MAX_HOST_NAME = 64;
const MAX_PASSWORD = 128;
const HOST_GRACE_MS = 30_000;

interface RoomEnv {
  ROOM_REGISTRY: DurableObjectNamespace;
}

export interface RoomMeta {
  code: string;
  name: string;
  map: string;
  game: string;
  hostName: string;
  players: number;
  maxPlayers: number;
  hasPassword: boolean;
  passwordHash?: string;
  hostSubject: string;
  createdAt: number;
  lastHeartbeat: number;
  hostGraceUntil: number;
}

interface SessionAttachment {
  version: 1;
  subject: string;
  role: 'host' | 'client';
  peerId: number;
}

interface Session extends SessionAttachment {
  socket: WebSocket;
}

function text(value: unknown, fallback: string, maxLength: number): string {
  if (typeof value !== 'string') return fallback;
  const trimmed = value.trim();
  return trimmed ? trimmed.slice(0, maxLength) : fallback;
}

function maxPlayers(value: unknown): number {
  const n = Number(value);
  if (!Number.isFinite(n)) return 16;
  return Math.min(32, Math.max(2, Math.trunc(n)));
}

function isUpgrade(request: Request): boolean {
  return request.method === 'GET' && request.headers.get('Upgrade')?.toLowerCase() === 'websocket';
}

export class MAGDRoomObject {
  private readonly sessions = new Map<WebSocket, Session>();
  private meta: RoomMeta | null = null;

  constructor(
    private readonly ctx: DurableObjectState,
    private readonly env: RoomEnv,
  ) {
    this.ctx.blockConcurrencyWhile(async () => {
      this.meta = await this.ctx.storage.get<RoomMeta>(ROOM_META_KEY) ?? null;
      this.restoreSessions();
      this.syncPlayerCount();
    });
  }

  private restoreSessions(): void {
    for (const ws of this.ctx.getWebSockets()) {
      const attachment = ws.deserializeAttachment() as SessionAttachment | null;
      if (!attachment || attachment.version !== 1 ||
          (attachment.role !== 'host' && attachment.role !== 'client') ||
          typeof attachment.subject !== 'string' || !attachment.subject ||
          !Number.isInteger(attachment.peerId) || attachment.peerId < 0 || attachment.peerId > 31) {
        try { ws.close(1008, 'Invalid session'); } catch {}
        continue;
      }
      this.sessions.set(ws, { ...attachment, socket: ws });
    }
  }

  private syncPlayerCount(): void {
    if (this.meta) this.meta.players = this.sessions.size;
  }

  private async saveMeta(): Promise<void> {
    if (this.meta) await this.ctx.storage.put(ROOM_META_KEY, this.meta);
    else await this.ctx.storage.delete(ROOM_META_KEY);
  }

  private safeMeta(): Omit<RoomMeta, 'passwordHash' | 'hostSubject' | 'hostGraceUntil'> & { hostConnected: boolean } | null {
    if (!this.meta) return null;
    const {
      passwordHash: _passwordHash,
      hostSubject: _hostSubject,
      hostGraceUntil: _hostGraceUntil,
      ...safe
    } = this.meta;
    return { ...safe, hostConnected: !!this.findHost() };
  }

  private findHost(): Session | undefined {
    for (const session of this.sessions.values()) {
      if (session.role === 'host') return session;
    }
    return undefined;
  }

  private findClientByPeerId(peerId: number): Session | undefined {
    for (const session of this.sessions.values()) {
      if (session.role !== 'host' && session.peerId === peerId) return session;
    }
    return undefined;
  }

  private findPeerId(): number {
    if (!this.meta) return -1;
    const used = new Set<number>();
    for (const session of this.sessions.values()) {
      if (session.role === 'client') used.add(session.peerId);
    }
    const max = Math.min(31, this.meta.maxPlayers - 1);
    for (let peerId = 1; peerId <= max; peerId++) {
      if (!used.has(peerId)) return peerId;
    }
    return -1;
  }

  private async destroyRoom(reason: string): Promise<void> {
    const code = this.meta?.code ?? null;
    for (const ws of this.sessions.keys()) {
      try { ws.close(1001, reason); } catch {}
    }
    this.sessions.clear();
    this.meta = null;
    if (code) {
      try {
        const id = this.env.ROOM_REGISTRY.idFromName('global');
        const stub = this.env.ROOM_REGISTRY.get(id);
        await stub.fetch(new Request('https://internal/unregister', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ code }),
        }));
      } catch {}
    }
    await this.ctx.storage.deleteAll();
  }

  private async relayGameDatagram(ws: WebSocket, message: ArrayBuffer): Promise<void> {
    const header = parseHeader(message);
    if (!header || header.type !== MagdMessageType.GAME_DATAGRAM) return;

    const session = this.sessions.get(ws);
    if (!session || !this.meta) return;

    const packet = parseGameDatagram(message);
    if (!packet || packet.data.byteLength > MAX_PACKET_SIZE) return;

    this.meta.lastHeartbeat = Date.now();

    if (session.role === 'host') {
      if ((packet.flags & GAME_HAS_TARGET) !== 0 && packet.peerId !== GAME_BROADCAST) {
        if (packet.peerId < 1 || packet.peerId > 31) return;
        const target = this.findClientByPeerId(packet.peerId);
        if (!target) return;
        try {
          target.socket.send(createGameDatagram(0, 0, packet.data));
        } catch {}
        await this.saveMeta();
        return;
      }

      for (const peer of this.sessions.values()) {
        if (peer.role === 'client') {
          try {
            peer.socket.send(createGameDatagram(0, 0, packet.data));
          } catch {}
        }
      }
      await this.saveMeta();
      return;
    }

    const host = this.findHost();
    if (!host) return;

    try {
      host.socket.send(createGameDatagram(GAME_HAS_SENDER, session.peerId, packet.data));
    } catch {}
    await this.saveMeta();
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === '/destroy' && request.method === 'POST') {
      if (request.headers.get('X-MAGD-Internal') !== '1') return new Response('Forbidden', { status: 403 });
      await this.destroyRoom('Room destroyed');
      return new Response('ok');
    }

    if (url.pathname === '/init' && request.method === 'POST') {
      if (request.headers.get('X-MAGD-Internal') !== '1') return new Response('Forbidden', { status: 403 });
      if (this.meta) return new Response(JSON.stringify({ error: 'Room already exists' }), { status: 409 });

      const body = await request.json().catch(() => null) as Record<string, unknown> | null;
      const code = text(body?.code, '', 64).toUpperCase();
      const hostSubject = text(body?.hostSubject, '', 128);
      const password = typeof body?.password === 'string' ? body.password : '';
      if (!/^[A-Z0-9_-]{3,64}$/.test(code) || !hostSubject) {
        return new Response(JSON.stringify({ error: 'Invalid room parameters' }), { status: 400 });
      }
      if (password.length > MAX_PASSWORD) return new Response('Password too long', { status: 400 });

      this.meta = {
        code,
        name: text(body?.name, 'MAGD Server', MAX_ROOM_NAME),
        map: text(body?.map, 'crossfire', MAX_MAP_NAME),
        game: text(body?.game, 'valve', MAX_GAME_NAME),
        hostName: text(body?.hostName, 'Host', MAX_HOST_NAME),
        players: 0,
        maxPlayers: maxPlayers(body?.maxPlayers),
        hasPassword: password.length > 0,
        passwordHash: password ? await hashPassword(password) : undefined,
        hostSubject,
        createdAt: Date.now(),
        lastHeartbeat: Date.now(),
        hostGraceUntil: 0,
      };
      await this.saveMeta();

      return new Response(JSON.stringify(this.safeMeta()), {
        status: 201,
        headers: { 'Content-Type': 'application/json; charset=utf-8' },
      });
    }

    if (url.pathname === '/info' && request.method === 'GET') {
      if (!this.meta) return new Response(JSON.stringify({ error: 'Room not found' }), { status: 404 });
      this.syncPlayerCount();
      return new Response(JSON.stringify(this.safeMeta()), {
        headers: { 'Content-Type': 'application/json; charset=utf-8' },
      });
    }

    if (url.pathname === '/ws') {
      if (!isUpgrade(request)) return new Response('Expected WebSocket', { status: 426 });
      if (!this.meta) return new Response('Room not found', { status: 404 });

      const subject = request.headers.get('X-MAGD-Subject');
      const role = request.headers.get('X-MAGD-Role');
      const password = request.headers.get('X-MAGD-Room-Password') ?? '';
      if (!subject || (role !== 'host' && role !== 'client')) return new Response('Unauthorized', { status: 401 });

      if (role === 'host') {
        if (subject !== this.meta.hostSubject) return new Response('Forbidden', { status: 403 });
        if (this.findHost()) return new Response('Host already connected', { status: 409 });
      } else {
        if (!this.findHost()) return new Response('Host offline', { status: 409 });
        if (this.sessions.size >= this.meta.maxPlayers) return new Response('Room full', { status: 409 });
        if (this.meta.hasPassword) {
          if (!password || !this.meta.passwordHash || await hashPassword(password) !== this.meta.passwordHash) {
            return new Response('Forbidden', { status: 403 });
          }
        }
        if ([...this.sessions.values()].some((session) => session.subject === subject)) {
          return new Response('Client already connected', { status: 409 });
        }
      }

      const peerId = role === 'host' ? 0 : this.findPeerId();
      if (role === 'client' && peerId < 1) return new Response('No peer slot available', { status: 409 });

      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      const attachment: SessionAttachment = {
        version: 1,
        subject,
        role,
        peerId,
      };

      this.ctx.acceptWebSocket(server);
      server.serializeAttachment(attachment);
      this.sessions.set(server, { ...attachment, socket: server });
      this.syncPlayerCount();
      this.meta.lastHeartbeat = Date.now();
      this.meta.hostGraceUntil = 0;
      await this.ctx.storage.deleteAlarm();
      await this.saveMeta();

      try {
        server.send(createMessage(MagdMessageType.WELCOME,
          new TextEncoder().encode(JSON.stringify({
            clientId: subject,
            peerId,
            isHost: role === 'host',
            code: this.meta.code,
            virtualHost: '10.254.0.1:27015',
          }))));
      } catch {
        this.sessions.delete(server);
        this.syncPlayerCount();
        await this.saveMeta();
        try { server.close(1011, 'Welcome failed'); } catch {}
        return new Response('WebSocket initialization failed', { status: 500 });
      }

      return new Response(null, { status: 101, webSocket: client });
    }

    return new Response('Not found', { status: 404 });
  }

  async webSocketMessage(ws: WebSocket, message: ArrayBuffer | string): Promise<void> {
    if (typeof message === 'string') return;
    if (message.byteLength > MAX_PACKET_SIZE + 7) return;

    const header = parseHeader(message);
    if (!header) return;

    if (header.type === MagdMessageType.PING) {
      try {
        const payload = new Uint8Array(message, 5, header.length).slice();
        ws.send(createMessage(MagdMessageType.PONG, payload));
      } catch {}
      return;
    }
    if (header.type === MagdMessageType.READY) return;
    if (header.type !== MagdMessageType.GAME_DATAGRAM) return;
    await this.relayGameDatagram(ws, message);
  }

  async webSocketClose(ws: WebSocket): Promise<void> {
    const session = this.sessions.get(ws);
    if (!session) return;
    this.sessions.delete(ws);
    if (!this.meta) return;

    if (session.role === 'host') {
      this.meta.hostGraceUntil = Date.now() + HOST_GRACE_MS;
      this.meta.lastHeartbeat = Date.now();
      await this.saveMeta();
      await this.ctx.storage.setAlarm(this.meta.hostGraceUntil);
      return;
    }

    this.syncPlayerCount();
    this.meta.lastHeartbeat = Date.now();
    await this.saveMeta();
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    try { ws.close(1011, 'WebSocket error'); } catch {}
  }

  async alarm(): Promise<void> {
    if (!this.meta) return;
    if (this.findHost()) return;

    if (this.meta.hostGraceUntil > Date.now()) {
      await this.ctx.storage.setAlarm(this.meta.hostGraceUntil);
      return;
    }

    await this.destroyRoom('Host disconnected');
  }
}
