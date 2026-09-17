const REGISTRY_KEY = 'rooms';

const MAX_REGISTERED_ROOMS = 10_000;

interface RegistryState {
  rooms: string[];
}

export class MAGDRegistryObject {
  constructor(
    private readonly state: DurableObjectState,
  ) {}

  private async load(): Promise<RegistryState> {
    return (
      await this.state.storage.get<RegistryState>(
        REGISTRY_KEY,
      )
    ) ?? {
      rooms: [],
    };
  }

  private async save(
    state: RegistryState,
  ): Promise<void> {
    await this.state.storage.put(
      REGISTRY_KEY,
      state,
    );
  }

  async fetch(
    request: Request,
  ): Promise<Response> {
    const url = new URL(request.url);
    const method = request.method.toUpperCase();

    /*
     * REGISTER ROOM
     */
    if (
      url.pathname === '/register' &&
      method === 'POST'
    ) {
      const body = await request
        .json()
        .catch(() => null) as {
          code?: unknown;
        } | null;

      const code =
        typeof body?.code === 'string'
          ? body.code.trim()
          : '';

      if (!code) {
        return new Response(
          'Missing code',
          { status: 400 },
        );
      }

      const state = await this.load();

      if (!state.rooms.includes(code)) {
        if (
          state.rooms.length >=
          MAX_REGISTERED_ROOMS
        ) {
          return new Response(
            'Registry full',
            { status: 507 },
          );
        }

        state.rooms.push(code);

        await this.save(state);
      }

      return new Response('ok');
    }

    /*
     * UNREGISTER ROOM
     */
    if (
      url.pathname === '/unregister' &&
      method === 'POST'
    ) {
      const body = await request
        .json()
        .catch(() => null) as {
          code?: unknown;
        } | null;

      const code =
        typeof body?.code === 'string'
          ? body.code.trim()
          : '';

      if (!code) {
        return new Response(
          'Missing code',
          { status: 400 },
        );
      }

      const state = await this.load();

      const nextRooms =
        state.rooms.filter(
          (entry) => entry !== code,
        );

      if (
        nextRooms.length !==
        state.rooms.length
      ) {
        state.rooms = nextRooms;

        await this.save(state);
      }

      return new Response('ok');
    }

    /*
     * LIST ROOMS
     */
    if (
      url.pathname === '/list' &&
      method === 'GET'
    ) {
      const state = await this.load();

      return new Response(
        JSON.stringify({
          rooms: state.rooms,
        }),
        {
          headers: {
            'Content-Type':
              'application/json',
          },
        },
      );
    }

    return new Response(
      'Not found',
      { status: 404 },
    );
  }
}