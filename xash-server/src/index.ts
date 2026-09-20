import { MAGDRoomObject } from './room.ts';
import { MAGDRegistryObject } from './registry.ts';
import { generateToken, verifyToken, type MagdTokenPayload } from './auth.ts';

export { MAGDRoomObject, MAGDRegistryObject };

export interface Env {
  ROOM_OBJECT: DurableObjectNamespace;
  ROOM_REGISTRY: DurableObjectNamespace;
  MAGD_JWT_SECRET?: string;
}

const CORS_HEADERS: Record<string, string> = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-MAGD-Room-Password',
  'Access-Control-Max-Age': '86400',
};

const COMMON_HEADERS: Record<string, string> = {
  'X-Content-Type-Options': 'nosniff',
};

function json(value: unknown, status = 200, extra: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(value), {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-store',
      ...COMMON_HEADERS,
      ...CORS_HEADERS,
      ...extra,
    },
  });
}

async function secret(env: Env): Promise<string> {
  if (!env.MAGD_JWT_SECRET) throw new Error('MAGD_JWT_SECRET is not configured');
  return env.MAGD_JWT_SECRET;
}

function normalizeRoomCode(value: unknown): string {
  const code = typeof value === 'string' ? value.trim().toUpperCase() : '';
  if (!/^[A-Z0-9_-]{3,64}$/.test(code)) throw new Error('Invalid room code');
  return code;
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

function bearer(request: Request): string | null {
  const value = request.headers.get('Authorization');
  if (!value) return null;
  const match = value.match(/^Bearer\s+(.+)$/i);
  return match ? match[1].trim() : null;
}

function roomStub(env: Env, code: string): DurableObjectStub {
  return env.ROOM_OBJECT.get(env.ROOM_OBJECT.idFromName(code));
}

function registryStub(env: Env): DurableObjectStub {
  return env.ROOM_REGISTRY.get(env.ROOM_REGISTRY.idFromName('global'));
}

async function readJsonObject(request: Request): Promise<Record<string, unknown>> {
  if (request.method !== 'POST') return {};
  const body = await request.json().catch(() => null);
  return body && typeof body === 'object' ? body as Record<string, unknown> : {};
}

async function createRoom(request: Request, env: Env, url: URL): Promise<Response> {
  const signingSecret = await secret(env);
  const body = await readJsonObject(request);

  let code: string;
  try {
    code = normalizeRoomCode(body.code ?? url.searchParams.get('code') ?? `MAGD-${crypto.randomUUID().slice(0, 4).toUpperCase()}`);
  } catch (error) {
    return json({ error: error instanceof Error ? error.message : 'Invalid room code' }, 400);
  }

  const passwordValue = body.password ?? url.searchParams.get('password') ?? '';
  const password = typeof passwordValue === 'string' ? passwordValue : '';
  if (password.length > 128) return json({ error: 'Password too long' }, 400);

  const room = roomStub(env, code);
  const hostSubject = crypto.randomUUID();
  const expiresAt = Date.now() + 30 * 24 * 60 * 60 * 1000;
  const hostPayload: MagdTokenPayload = {
    sub: hostSubject,
    exp: expiresAt,
    role: 'host',
    room: code,
  };

  const hostToken = await generateToken(hostPayload, signingSecret);

  const initResponse = await room.fetch(new Request('https://internal/init', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-MAGD-Internal': '1',
    },
    body: JSON.stringify({
      code,
      name: text(body.name ?? url.searchParams.get('name'), 'MAGD Server', 64),
      map: text(body.map ?? url.searchParams.get('map'), 'crossfire', 64),
      game: text(body.game ?? url.searchParams.get('game'), 'valve', 32),
      hostName: text(body.hostName ?? url.searchParams.get('host'), 'Host', 64),
      maxPlayers: maxPlayers(body.maxPlayers ?? url.searchParams.get('maxPlayers')),
      password,
      hostSubject,
    }),
  }));

  if (!initResponse.ok) {
    return new Response(await initResponse.text(), {
      status: initResponse.status,
      headers: { 'Content-Type': 'application/json; charset=utf-8', ...CORS_HEADERS },
    });
  }

  const register = await registryStub(env).fetch(new Request('https://internal/register', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ code }),
  }));

  if (!register.ok) {
    await room.fetch(new Request('https://internal/destroy', {
      method: 'POST',
      headers: { 'X-MAGD-Internal': '1' },
    })).catch(() => undefined);
    return json({ error: 'Failed to register room' }, 500);
  }

  const roomInfo = await initResponse.json();
  return json({ code, hostToken, expiresAt, room: roomInfo }, 201);
}

async function authenticateGuest(request: Request, env: Env, url: URL): Promise<Response> {
  const codeValue = request.method === 'POST'
    ? ((await request.json().catch(() => null) as Record<string, unknown> | null)?.room)
    : url.searchParams.get('room');

  let code: string;
  try {
    code = normalizeRoomCode(codeValue);
  } catch (error) {
    return json({ error: error instanceof Error ? error.message : 'Invalid room code' }, 400);
  }

  const roomInfo = await roomStub(env, code).fetch(new Request('https://internal/info'));
  if (!roomInfo.ok) return json({ error: 'Room not found' }, 404);

  const signingSecret = await secret(env);
  const expiresAt = Date.now() + 7 * 24 * 60 * 60 * 1000;
  const payload: MagdTokenPayload = {
    sub: crypto.randomUUID(),
    exp: expiresAt,
    role: 'client',
    room: code,
  };
  const token = await generateToken(payload, signingSecret);
  return json({ token, expiresAt, room: code });
}

async function listRooms(env: Env): Promise<Response> {
  const response = await registryStub(env).fetch(new Request('https://internal/list'));
  if (!response.ok) return json({ error: 'Room registry unavailable' }, 503);

  const data = await response.json() as { rooms?: unknown };
  const codes = Array.isArray(data.rooms) ? data.rooms.filter((value): value is string => typeof value === 'string') : [];
  const rooms = (await Promise.all(codes.map(async (code) => {
    try {
      const room = await roomStub(env, code).fetch(new Request('https://internal/info'));
      if (room.ok) return await room.json();
      if (room.status === 404) {
        await registryStub(env).fetch(new Request('https://internal/unregister', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ code }),
        }));
      }
    } catch {}
    return null;
  }))).filter((value) => value !== null);

  return json({ rooms });
}

async function roomInfo(env: Env, code: string): Promise<Response> {
  const response = await roomStub(env, code).fetch(new Request('https://internal/info'));
  return new Response(await response.text(), {
    status: response.status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-store',
      ...COMMON_HEADERS,
      ...CORS_HEADERS,
    },
  });
}

async function roomWebSocket(request: Request, env: Env, code: string, url: URL): Promise<Response> {
  if (request.method !== 'GET' || request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') {
    return new Response('Expected WebSocket', { status: 426, headers: CORS_HEADERS });
  }

  const token = bearer(request) ?? url.searchParams.get('token');
  if (!token) return new Response('Unauthorized', { status: 401, headers: CORS_HEADERS });

  const payload = await verifyToken(token, await secret(env));
  if (!payload || payload.room !== code) {
    return new Response('Unauthorized', { status: 401, headers: CORS_HEADERS });
  }

  const headers = new Headers(request.headers);
  headers.delete('Authorization');
  headers.delete('Cookie');
  headers.delete('X-MAGD-Subject');
  headers.delete('X-MAGD-Role');

  const password = request.headers.get('X-MAGD-Room-Password') ?? url.searchParams.get('password');
  if (password) headers.set('X-MAGD-Room-Password', password);
  headers.set('X-MAGD-Subject', payload.sub);
  headers.set('X-MAGD-Role', payload.role);

  const cleanUrl = new URL(request.url);
  cleanUrl.pathname = `/internal-room/${encodeURIComponent(code)}`;
  cleanUrl.search = '';

  /*
   * Keep the WebSocket negotiation headers intact while removing credentials
   * from the URL. The Durable Object receives the same upgrade semantics.
   */
  const internalRequest = new Request('https://internal/ws', {
    method: 'GET',
    headers,
  });
  const stub = roomStub(env, code);
  return stub.fetch(internalRequest);
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }

    try {
      if (url.pathname === '/api/v1/health' && request.method === 'GET') {
        return json({ status: 'ok', platform: 'MAGD Online Platform', version: '2.0.0' });
      }

      if (url.pathname === '/api/v1/auth/guest' && (request.method === 'GET' || request.method === 'POST')) {
        return authenticateGuest(request, env, url);
      }

      if (url.pathname === '/api/v1/rooms/create' && (request.method === 'GET' || request.method === 'POST')) {
        return createRoom(request, env, url);
      }

      if (url.pathname === '/api/v1/rooms' && request.method === 'GET') {
        return listRooms(env);
      }

      const infoMatch = url.pathname.match(/^\/api\/v1\/rooms\/info\/([A-Z0-9_-]{3,64})$/i);
      if (infoMatch && request.method === 'GET') {
        return roomInfo(env, normalizeRoomCode(infoMatch[1]));
      }

      const wsMatch = url.pathname.match(/^\/ws\/room\/([A-Z0-9_-]{3,64})$/i);
      if (wsMatch) {
        return roomWebSocket(request, env, normalizeRoomCode(wsMatch[1]), url);
      }

      return new Response('Not found', { status: 404, headers: CORS_HEADERS });
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Internal server error';
      console.error('[MAGD]', message);
      return json({ error: message }, 500);
    }
  },
} satisfies ExportedHandler<Env>;
