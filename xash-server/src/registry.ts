const REGISTRY_KEY = 'rooms';
const MAX_REGISTERED_ROOMS = 10_000;
const ROOM_CODE_PATTERN = /^[A-Z0-9_-]{3,64}$/;

interface RegistryState {
  rooms: string[];
}

function validRoomCode(value: unknown): value is string {
  return typeof value === 'string' && ROOM_CODE_PATTERN.test(value);
}

export class MAGDRegistryObject {
  constructor(private readonly state: DurableObjectState) {}

  private async load(): Promise<RegistryState> {
    return await this.state.storage.get<RegistryState>(REGISTRY_KEY) ?? { rooms: [] };
  }

  private async save(state: RegistryState): Promise<void> {
    await this.state.storage.put(REGISTRY_KEY, state);
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const method = request.method.toUpperCase();

    if (url.pathname === '/register' && method === 'POST') {
      const body = await request.json().catch(() => null) as { code?: unknown } | null;
      const code = typeof body?.code === 'string' ? body.code.trim().toUpperCase() : '';
      if (!validRoomCode(code)) return new Response('Invalid code', { status: 400 });

      const state = await this.load();
      if (!state.rooms.includes(code)) {
        if (state.rooms.length >= MAX_REGISTERED_ROOMS) {
          return new Response('Registry full', { status: 507 });
        }
        state.rooms.push(code);
        await this.save(state);
      }
      return new Response('ok');
    }

    if (url.pathname === '/unregister' && method === 'POST') {
      const body = await request.json().catch(() => null) as { code?: unknown } | null;
      const code = typeof body?.code === 'string' ? body.code.trim().toUpperCase() : '';
      if (!validRoomCode(code)) return new Response('Invalid code', { status: 400 });

      const state = await this.load();
      const rooms = state.rooms.filter((entry) => entry !== code);
      if (rooms.length !== state.rooms.length) {
        state.rooms = rooms;
        await this.save(state);
      }
      return new Response('ok');
    }

    if (url.pathname === '/list' && method === 'GET') {
      const state = await this.load();
      return new Response(JSON.stringify({ rooms: state.rooms }), {
        headers: { 'Content-Type': 'application/json; charset=utf-8' },
      });
    }

    return new Response('Not found', { status: 404 });
  }
}
