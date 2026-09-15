import { MAGDRoomObject } from './room';

export { MAGDRoomObject };

export interface Env {
  ROOM_OBJECT: DurableObjectNamespace;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    // CORS Headers
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

    // Connect to Room WebSocket
    if (url.pathname.startsWith('/ws/room/')) {
      const roomCode = url.pathname.replace('/ws/room/', '');
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
