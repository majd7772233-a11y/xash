import {
  MAGDRoomObject,
} from './room';

import {
  MAGDRegistryObject,
} from './registry';

import {
  generateToken,
  verifyToken,
  type MagdTokenPayload,
} from './auth';

export {
  MAGDRoomObject,
  MAGDRegistryObject,
};

export interface Env {
  ROOM_OBJECT:
    DurableObjectNamespace;

  ROOM_REGISTRY:
    DurableObjectNamespace;

  MAGD_JWT_SECRET?:
    string;
}

const CORS_HEADERS:
  Record<string, string> = {
    'Access-Control-Allow-Origin':
      '*',

    'Access-Control-Allow-Methods':
      'GET, POST, OPTIONS',

    'Access-Control-Allow-Headers':
      'Content-Type, Authorization, X-MAGD-Room-Password',

    'Access-Control-Max-Age':
      '86400',
  };

const json = (
  value: unknown,
  status = 200,
): Response =>
  new Response(
    JSON.stringify(value),
    {
      status,

      headers: {
        'Content-Type':
          'application/json; charset=utf-8',

        ...CORS_HEADERS,
      },
    },
  );

function registry(
  env: Env,
): DurableObjectStub {
  const id =
    env.ROOM_REGISTRY.idFromName(
      'global',
    );

  return env.ROOM_REGISTRY.get(
    id,
  );
}

async function getSecret(
  env: Env,
): Promise<string> {
  if (!env.MAGD_JWT_SECRET) {
    throw new Error(
      'Server authentication secret is not configured',
    );
  }

  return env.MAGD_JWT_SECRET;
}

function normalizeRoomCode(
  value: unknown,
): string {
  const code =
    typeof value === 'string'
      ? value.trim().toUpperCase()
      : '';

  if (
    !/^[A-Z0-9_-]{3,64}$/.test(
      code,
    )
  ) {
    throw new Error(
      'Invalid room code',
    );
  }

  return code;
}

function stringField(
  value: unknown,
  fallback: string,
  maxLength: number,
): string {
  if (
    typeof value !== 'string'
  ) {
    return fallback;
  }

  const trimmed =
    value.trim();

  if (!trimmed) {
    return fallback;
  }

  return trimmed.slice(
    0,
    maxLength,
  );
}

function maxPlayersField(
  value: unknown,
): number {
  const n = Number(value);

  if (!Number.isFinite(n)) {
    return 16;
  }

  return Math.min(
    32,
    Math.max(
      2,
      Math.trunc(n),
    ),
  );
}

function bearerToken(
  request: Request,
): string | null {
  const authorization =
    request.headers.get(
      'Authorization',
    );

  if (!authorization) {
    return null;
  }

  const match =
    authorization.match(
      /^Bearer\s+(.+)$/i,
    );

  return match
    ? match[1].trim()
    : null;
}

function getRoomStub(
  env: Env,
  code: string,
): DurableObjectStub {
  const id =
    env.ROOM_OBJECT.idFromName(
      code,
    );

  return env.ROOM_OBJECT.get(
    id,
  );
}

async function createRoom(
  request: Request,
  env: Env,
  url: URL,
): Promise<Response> {
  const secret =
    await getSecret(env);

  let body:
    Record<string, unknown> = {};

  if (
    request.method ===
    'POST'
  ) {
    const parsed =
      await request
        .json()
        .catch(() => null);

    if (
      parsed !== null &&
      typeof parsed === 'object'
    ) {
      body =
        parsed as Record<
          string,
          unknown
        >;
    }
  }

  const code =
    normalizeRoomCode(
      body.code ??
      url.searchParams.get(
        'code',
      ) ??
      `MAGD-${crypto.randomUUID().slice(0, 4).toUpperCase()}`,
    );

  const passwordValue =
    body.password ??
    url.searchParams.get(
      'password',
    ) ??
    '';

  const password =
    typeof passwordValue ===
    'string'
      ? passwordValue
      : '';

  if (
    password.length > 128
  ) {
    return json(
      {
        error:
          'Password too long',
      },
      400,
    );
  }

  /*
   * Unique identity representing
   * the room owner.
   */
  const hostSubject =
    crypto.randomUUID();

  const expiresAt =
    Date.now() +
    86_400_000;

  const hostPayload:
    MagdTokenPayload = {
    sub:
      hostSubject,

    exp:
      expiresAt,

    role:
      'host',

    room:
      code,
  };

  const room =
    getRoomStub(
      env,
      code,
    );

  /*
   * Initialize room metadata.
   */
  const initResponse =
    await room.fetch(
      new Request(
        'https://internal/init',
        {
          method: 'POST',

          headers: {
            'Content-Type':
              'application/json',

            /*
             * Internal-only guard.
             */
            'X-MAGD-Internal':
              '1',
          },

          body:
            JSON.stringify({
              code,

              name:
                stringField(
                  body.name ??
                    url.searchParams.get(
                      'name',
                    ),
                  'MAGD Server',
                  64,
                ),

              map:
                stringField(
                  body.map ??
                    url.searchParams.get(
                      'map',
                    ),
                  'crossfire',
                  64,
                ),

              game:
                stringField(
                  body.game ??
                    url.searchParams.get(
                      'game',
                    ),
                  'valve',
                  32,
                ),

              hostName:
                stringField(
                  body.hostName ??
                    url.searchParams.get(
                      'host',
                    ),
                  'Host',
                  64,
                ),

              maxPlayers:
                maxPlayersField(
                  body.maxPlayers ??
                    url.searchParams.get(
                      'maxPlayers',
                    ),
                ),

              password,

              hostSubject,
            }),
        },
      ),
    );

  if (!initResponse.ok) {
    const message =
      await initResponse.text();

    return new Response(
      message,
      {
        status:
          initResponse.status,

        headers: {
          'Content-Type':
            'application/json; charset=utf-8',

          ...CORS_HEADERS,
        },
      },
    );
  }

  const roomInfo =
    await initResponse.json();

  /*
   * Register room globally for
   * room discovery.
   */
  const registerResponse =
    await registry(env).fetch(
      new Request(
        'https://internal/register',
        {
          method:
            'POST',

          headers: {
            'Content-Type':
              'application/json',
          },

          body:
            JSON.stringify({
              code,
            }),
        },
      ),
    );

  if (
    !registerResponse.ok
  ) {
    /*
     * Roll back room creation.
     */
    await room
      .fetch(
        new Request(
          'https://internal/destroy',
          {
            method:
              'POST',

            headers: {
              'X-MAGD-Internal':
                '1',
            },
          },
        ),
      )
      .catch(
        () => undefined,
      );

    return json(
      {
        error:
          'Failed to register room',
      },
      500,
    );
  }

  const hostToken =
    await generateToken(
      hostPayload,
      secret,
    );

  return json(
    {
      code,

      hostToken,

      expiresAt,

      room:
        roomInfo,
    },
    201,
  );
}

async function listRooms(
  env: Env,
): Promise<Response> {
  const listResponse =
    await registry(env).fetch(
      new Request(
        'https://internal/list',
      ),
    );

  if (
    !listResponse.ok
  ) {
    return json(
      {
        error:
          'Room registry unavailable',
      },
      503,
    );
  }

  const registryData =
    await listResponse.json()
      as {
        rooms?: unknown;
      };

  const codes =
    Array.isArray(
      registryData.rooms,
    )
      ? registryData.rooms.filter(
          (
            value,
          ): value is string =>
            typeof value ===
            'string',
        )
      : [];

  /*
   * Query rooms in parallel.
   */
  const results =
    await Promise.all(
      codes.map(
        async (code) => {
          try {
            const response =
              await getRoomStub(
                env,
                code,
              ).fetch(
                new Request(
                  'https://internal/info',
                ),
              );

            if (
              response.ok
            ) {
              return {
                code,
                data:
                  await response.json(),
              };
            }

            /*
             * Remove stale room
             * entries.
             */
            if (
              response.status ===
              404
            ) {
              await registry(
                env,
              ).fetch(
                new Request(
                  'https://internal/unregister',
                  {
                    method:
                      'POST',

                    headers: {
                      'Content-Type':
                        'application/json',
                    },

                    body:
                      JSON.stringify({
                        code,
                      }),
                  },
                ),
              );
            }
          } catch {}

          return null;
        },
      ),
    );

  return json(
    {
      rooms:
        results
          .filter(
            (
              entry,
            ): entry is {
              code: string;
              data: unknown;
            } =>
              entry !== null,
          )
          .map(
            (
              entry,
            ) =>
              entry.data,
          ),
    },
  );
}

async function roomInfo(
  env: Env,
  code: string,
): Promise<Response> {
  const response =
    await getRoomStub(
      env,
      code,
    ).fetch(
      new Request(
        'https://internal/info',
      ),
    );

  return new Response(
    await response.text(),
    {
      status:
        response.status,

      headers: {
        'Content-Type':
          'application/json; charset=utf-8',

        ...CORS_HEADERS,
      },
    },
  );
}

async function roomWebSocket(
  request: Request,
  env: Env,
  code: string,
  url: URL,
): Promise<Response> {
  if (
    request.headers
      .get('Upgrade')
      ?.toLowerCase() !==
    'websocket'
  ) {
    return new Response(
      'Expected WebSocket',
      {
        status: 400,
        headers:
          CORS_HEADERS,
      },
    );
  }

  const secret =
    await getSecret(env);

  /*
   * Authorization header is preferred.
   *
   * Query token remains supported
   * for compatibility with the current
   * Xash C implementation.
   */
  const token =
    bearerToken(request) ??
    url.searchParams.get(
      'token',
    );

  if (!token) {
    return new Response(
      'Unauthorized',
      {
        status: 401,
        headers:
          CORS_HEADERS,
      },
    );
  }

  const payload =
    await verifyToken(
      token,
      secret,
    );

  if (!payload) {
    return new Response(
      'Unauthorized',
      {
        status: 401,
        headers:
          CORS_HEADERS,
      },
    );
  }

  /*
   * A host token is permanently
   * bound to its room.
   */
  if (
    payload.role ===
      'host' &&
    payload.room !==
      code
  ) {
    return new Response(
      'Forbidden',
      {
        status: 403,
        headers:
          CORS_HEADERS,
      },
    );
  }

  /*
   * Pass authenticated identity to
   * the Durable Object.
   */
  const headers =
    new Headers(
      request.headers,
    );

  headers.set(
    'X-MAGD-Subject',
    payload.sub,
  );

  headers.set(
    'X-MAGD-Role',
    payload.role,
  );

  /*
   * Prefer a header but keep query
   * parameter compatibility.
   */
  const password =
    request.headers.get(
      'X-MAGD-Room-Password',
    ) ??
    url.searchParams.get(
      'password',
    );

  if (password) {
    headers.set(
      'X-MAGD-Room-Password',
      password,
    );
  }

  /*
   * These credentials should not
   * be forwarded deeper into the DO.
   */
  headers.delete(
    'Authorization',
  );

  headers.delete(
    'Cookie',
  );

  /*
   * Rebuild the internal request
   * without the original URL query
   * string containing credentials.
   */
  const internalRequest =
    new Request(
      'https://internal/ws',
      {
        method: 'GET',
        headers,
      },
    );

  return getRoomStub(
    env,
    code,
  ).fetch(
    internalRequest,
  );
}

export default {
  async fetch(
    request: Request,
    env: Env,
  ): Promise<Response> {
    const url =
      new URL(request.url);

    if (
      request.method ===
      'OPTIONS'
    ) {
      return new Response(
        null,
        {
          status: 204,
          headers:
            CORS_HEADERS,
        },
      );
    }

    try {
      /*
       * HEALTH
       */
      if (
        url.pathname ===
          '/api/v1/health' &&
        request.method ===
          'GET'
      ) {
        return json({
          status:
            'ok',

          platform:
            'MAGD Online Platform',

          version:
            '1.2.0',

          transport:
            'wss',
        });
      }

      /*
       * GUEST AUTH
       */
      if (
        url.pathname ===
          '/api/v1/auth/guest' &&
        request.method ===
          'GET'
      ) {
        const secret =
          await getSecret(env);

        const payload:
          MagdTokenPayload = {
          sub:
            crypto.randomUUID(),

          exp:
            Date.now() +
            86_400_000,

          role:
            'client',
        };

        return json({
          token:
            await generateToken(
              payload,
              secret,
            ),

          expiresAt:
            payload.exp,
        });
      }

      /*
       * CREATE ROOM
       *
       * GET is retained for compatibility
       * with the current Xash client.
       *
       * POST is the preferred API.
       */
      if (
        url.pathname ===
          '/api/v1/rooms/create' &&
        (
          request.method ===
            'GET' ||
          request.method ===
            'POST'
        )
      ) {
        return await createRoom(
          request,
          env,
          url,
        );
      }

      /*
       * ROOM DISCOVERY
       */
      if (
        url.pathname ===
          '/api/v1/rooms' &&
        request.method ===
          'GET'
      ) {
        return await listRooms(
          env,
        );
      }

      /*
       * ROOM INFO
       */
      const infoPrefix =
        '/api/v1/rooms/info/';

      if (
        url.pathname.startsWith(
          infoPrefix,
        ) &&
        request.method ===
          'GET'
      ) {
        const code =
          normalizeRoomCode(
            decodeURIComponent(
              url.pathname.slice(
                infoPrefix.length,
              ),
            ),
          );

        return await roomInfo(
          env,
          code,
        );
      }

      /*
       * ROOM WEBSOCKET
       */
      const wsPrefix =
        '/ws/room/';

      if (
        url.pathname.startsWith(
          wsPrefix,
        ) &&
        request.method ===
          'GET'
      ) {
        const code =
          normalizeRoomCode(
            decodeURIComponent(
              url.pathname.slice(
                wsPrefix.length,
              ),
            ),
          );

        return await roomWebSocket(
          request,
          env,
          code,
          url,
        );
      }

      return json(
        {
          error:
            'Endpoint not found',
        },
        404,
      );
    } catch (error) {
      console.error(
        'MAGD request error',
        error,
      );

      /*
       * Do not expose internal
       * exceptions/secrets to clients.
       */
      return json(
        {
          error:
            'Internal server error',
        },
        500,
      );
    }
  },
};