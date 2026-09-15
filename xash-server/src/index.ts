import { MAGDRoomObject } from './room';

export { MAGDRoomObject };

export interface Env {
  ROOM_OBJECT: DurableObjectNamespace;
}

const activeRooms: Set<string> = new Set();

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    const corsHeaders = {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type, Authorization'
    };

    if (request.method === 'OPTIONS') {
      return new Response(null, { headers: corsHeaders });
    }

    // Health check
    if (url.pathname === '/api/v1/health') {
      return new Response(JSON.stringify({ status: 'ok', platform: 'MAGD Online Platform', version: '1.0.0' }), {
        headers: { 'Content-Type': 'application/json', ...corsHeaders }
      });
    }

    // Auth Guest Token
    if (url.pathname === '/api/v1/auth/guest') {
      const token = 'magd_token_' + crypto.randomUUID().replace(/-/g, '');
      return new Response(JSON.stringify({ token, expiresAt: Date.now() + 86400 * 1000 }), {
        headers: { 'Content-Type': 'application/json', ...corsHeaders }
      });
    }

    // Create Room Endpoint
    if (url.pathname === '/api/v1/rooms/create' && request.method === 'POST') {
      try {
        const body = await request.json() as any;
        const code = body.code || 'MAGD-' + Math.random().toString(36).substring(2, 6).toUpperCase();
        activeRooms.add(code);

        const id = env.ROOM_OBJECT.idFromName(code);
        const roomObject = env.ROOM_OBJECT.get(id);

        const initReq = new Request('http://internal/init', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            code,
            name: body.name || 'MAGD Server',
            map: body.map || 'crossfire',
            game: body.game || 'valve',
            hostName: body.hostName || 'Host',
            maxPlayers: body.maxPlayers || 16,
            hasPassword: !!body.password,
            password: body.password || undefined
          })
        });

        const res = await roomObject.fetch(initReq);
        const data = await res.json();

        return new Response(JSON.stringify({ code, room: data }), {
          status: 201,
          headers: { 'Content-Type': 'application/json', ...corsHeaders }
        });
      } catch (err: any) {
        return new Response(JSON.stringify({ error: err.message || 'Invalid request' }), {
          status: 400,
          headers: { 'Content-Type': 'application/json', ...corsHeaders }
        });
      }
    }

    // List Active Rooms
    if (url.pathname === '/api/v1/rooms' && request.method === 'GET') {
      const roomList = [];
      for (const code of activeRooms) {
        try {
          const id = env.ROOM_OBJECT.idFromName(code);
          const roomObject = env.ROOM_OBJECT.get(id);
          const res = await roomObject.fetch(new Request('http://internal/info'));
          if (res.status === 200) {
            const data = await res.json();
            roomList.push(data);
          } else {
            activeRooms.delete(code);
          }
        } catch {
          activeRooms.delete(code);
        }
      }

      return new Response(JSON.stringify({ rooms: roomList }), {
        headers: { 'Content-Type': 'application/json', ...corsHeaders }
      });
    }

    // Connect to Room WebSocket
    if (url.pathname.startsWith('/ws/room/')) {
      const roomCode = url.pathname.replace('/ws/room/', '');
      activeRooms.add(roomCode);
      const id = env.ROOM_OBJECT.idFromName(roomCode);
      const roomObject = env.ROOM_OBJECT.get(id);
      return roomObject.fetch(request);
    }

    // Get Room Info
    if (url.pathname.startsWith('/api/v1/rooms/info/')) {
      const roomCode = url.pathname.replace('/api/v1/rooms/info/', '');
      const id = env.ROOM_OBJECT.idFromName(roomCode);
      const roomObject = env.ROOM_OBJECT.get(id);
      const req = new Request(`http://internal/info`);
      const res = await roomObject.fetch(req);
      const data = await res.json();
      return new Response(JSON.stringify(data), {
        status: res.status,
        headers: { 'Content-Type': 'application/json', ...corsHeaders }
      });
    }

    return new Response(JSON.stringify({ error: 'Endpoint not found' }), {
      status: 404,
      headers: { 'Content-Type': 'application/json', ...corsHeaders }
    });
  }
};
