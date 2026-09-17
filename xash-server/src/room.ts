import {
  createGameDatagram,
  createMessage,
  GAME_HAS_TARGET,
  GAME_BROADCAST,
  MagdMessageType,
  parseGameDatagram,
  parseHeader,
} from './protocol';

import {
  hashPassword,
} from './auth';

const ROOM_META_KEY = 'meta';

const MAX_PACKET_SIZE = 16_384;

const MAX_ROOM_NAME = 64;
const MAX_MAP_NAME = 64;
const MAX_GAME_NAME = 32;
const MAX_HOST_NAME = 64;
const MAX_PASSWORD = 128;

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
}

interface SessionAttachment {
  version: 1;

  subject: string;

  role:
    | 'host'
    | 'client';

  /*
   * Host = 0
   * Client = 1..31
   */
  peerId: number;
}

interface Session
  extends SessionAttachment {
  socket: WebSocket;
}

function text(
  value: unknown,
  fallback: string,
  maxLength: number,
): string {
  if (
    typeof value !== 'string'
  ) {
    return fallback;
  }

  const trimmed = value.trim();

  if (!trimmed) {
    return fallback;
  }

  return trimmed.slice(
    0,
    maxLength,
  );
}

function clampMaxPlayers(
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

function isWebSocketUpgrade(
  request: Request,
): boolean {
  return (
    request.headers
      .get('Upgrade')
      ?.toLowerCase() ===
    'websocket'
  );
}

export class MAGDRoomObject {
  /*
   * This Map is only an in-memory index.
   *
   * For hibernation survival, every
   * connection also gets a serialized
   * attachment.
   */
  private readonly sessions =
    new Map<WebSocket, Session>();

  private meta:
    | RoomMeta
    | null = null;

  constructor(
    private readonly ctx:
      DurableObjectState,
  ) {
    /*
     * During constructor execution,
     * restore persistent metadata and
     * hibernating WebSocket sessions.
     */
    this.ctx.blockConcurrencyWhile(
      async () => {
        this.meta =
          await this.ctx.storage.get<RoomMeta>(
            ROOM_META_KEY,
          ) ?? null;

        this.restoreSessions();

        this.syncPlayerCount();
      },
    );
  }

  private restoreSessions(): void {
    for (
      const ws of this.ctx.getWebSockets()
    ) {
      const attachment =
        ws.deserializeAttachment() as
          | SessionAttachment
          | null;

      if (
        !attachment ||
        attachment.version !== 1
      ) {
        try {
          ws.close(
            1008,
            'Invalid session',
          );
        } catch {}

        continue;
      }

      if (
        (
          attachment.role !== 'host' &&
          attachment.role !== 'client'
        ) ||
        typeof attachment.subject !==
          'string' ||
        attachment.subject.length === 0 ||
        !Number.isInteger(
          attachment.peerId,
        ) ||
        attachment.peerId < 0 ||
        attachment.peerId > 255
      ) {
        try {
          ws.close(
            1008,
            'Invalid session',
          );
        } catch {}

        continue;
      }

      this.sessions.set(
        ws,
        {
          ...attachment,
          socket: ws,
        },
      );
    }
  }

  private syncPlayerCount(): void {
    if (!this.meta) {
      return;
    }

    this.meta.players =
      this.sessions.size;
  }

  private async saveMeta(): Promise<void> {
    if (this.meta) {
      await this.ctx.storage.put(
        ROOM_META_KEY,
        this.meta,
      );
    } else {
      await this.ctx.storage.delete(
        ROOM_META_KEY,
      );
    }
  }

  private safeMeta():
    | Omit<
        RoomMeta,
        'passwordHash' |
        'hostSubject'
      >
    | null {
    if (!this.meta) {
      return null;
    }

    const {
      passwordHash: _passwordHash,
      hostSubject: _hostSubject,
      ...safe
    } = this.meta;

    return safe;
  }

  private findPeerId(): number {
    if (!this.meta) {
      return -1;
    }

    const used =
      new Set<number>();

    for (
      const session
      of this.sessions.values()
    ) {
      if (
        session.role !== 'host'
      ) {
        used.add(
          session.peerId,
        );
      }
    }

    /*
     * 0 = host
     * 1..31 = clients
     */
    const maxPeerId = Math.min(
      31,
      this.meta.maxPlayers - 1,
    );

    for (
      let peerId = 1;
      peerId <= maxPeerId;
      peerId++
    ) {
      if (!used.has(peerId)) {
        return peerId;
      }
    }

    return -1;
  }

  private findHost():
    | Session
    | undefined {
    for (
      const session
      of this.sessions.values()
    ) {
      if (
        session.role === 'host'
      ) {
        return session;
      }
    }

    return undefined;
  }

  private findClientByPeerId(
    peerId: number,
  ):
    | Session
    | undefined {
    for (
      const session
      of this.sessions.values()
    ) {
      if (
        session.role !== 'host' &&
        session.peerId === peerId
      ) {
        return session;
      }
    }

    return undefined;
  }

  private async handleMessage(
    ws: WebSocket,
    message: ArrayBuffer,
  ): Promise<void> {
    /*
     * MAGD application packet:
     *
     * 5 byte header
     * + max 16 KiB game packet
     */
    if (
      message.byteLength < 5 ||
      message.byteLength >
        MAX_PACKET_SIZE + 7
    ) {
      return;
    }

    const header =
      parseHeader(message);

    if (!header) {
      return;
    }

    const payload =
      new Uint8Array(
        message,
        5,
        header.length,
      );

    /*
     * Application-level MAGD PING.
     */
    if (
      header.type ===
      MagdMessageType.PING
    ) {
      try {
        ws.send(
          createMessage(
            MagdMessageType.PONG,
            payload.slice(),
          ),
        );
      } catch {}

      return;
    }

    /*
     * READY is currently informational.
     */
    if (
      header.type ===
      MagdMessageType.READY
    ) {
      return;
    }

    /*
     * Only GAME_DATAGRAM participates
     * in the actual Xash relay.
     */
    if (
      header.type !==
      MagdMessageType.GAME_DATAGRAM
    ) {
      return;
    }

    const session =
      this.sessions.get(ws);

    if (!session || !this.meta) {
      return;
    }

    const packet =
      parseGameDatagram(message);

    if (
      !packet ||
      packet.data.byteLength >
        MAX_PACKET_SIZE
    ) {
      return;
    }

    /*
     * Do NOT write storage for every
     * game packet. That would be very
     * expensive for a real-time relay.
     */
    this.meta.lastHeartbeat =
      Date.now();

    /*
     * ===============================
     * HOST -> CLIENT(S)
     * ===============================
     */
    if (
      session.role === 'host'
    ) {
      const wantsTarget =
        (
          packet.flags &
          GAME_HAS_TARGET
        ) !== 0;

      /*
       * Targeted packet.
       */
      if (
        wantsTarget &&
        packet.peerId !==
          GAME_BROADCAST
      ) {
        if (
          packet.peerId < 1 ||
          packet.peerId > 31
        ) {
          return;
        }

        const target =
          this.findClientByPeerId(
            packet.peerId,
          );

        if (!target) {
          return;
        }

        try {
          /*
           * Remove sender/target metadata
           * before delivering to the client.
           */
          target.socket.send(
            createGameDatagram(
              0,
              0,
              packet.data,
            ),
          );
        } catch {}

        return;
      }

      /*
       * Broadcast.
       */
      for (
        const peer
        of this.sessions.values()
      ) {
        if (
          peer.role === 'host'
        ) {
          continue;
        }

        try {
          peer.socket.send(
            createGameDatagram(
              0,
              0,
              packet.data,
            ),
          );
        } catch {}
      }

      return;
    }

    /*
     * ===============================
     * CLIENT -> HOST
     * ===============================
     */

    const host =
      this.findHost();

    if (!host) {
      return;
    }

    try {
      host.socket.send(
        createGameDatagram(
          1,
          session.peerId,
          packet.data,
        ),
      );
    } catch {}
  }

  async fetch(
    request: Request,
  ): Promise<Response> {
    const url =
      new URL(request.url);

    /*
     * ==================================
     * INTERNAL ROOM DESTROY
     * ==================================
     *
     * Used only for rollback if registry
     * registration fails during creation.
     */
    if (
      url.pathname === '/destroy' &&
      request.method === 'POST'
    ) {
      if (
        request.headers.get(
          'X-MAGD-Internal',
        ) !== '1'
      ) {
        return new Response(
          'Forbidden',
          { status: 403 },
        );
      }

      for (
        const ws
        of this.sessions.keys()
      ) {
        try {
          ws.close(
            1001,
            'Room destroyed',
          );
        } catch {}
      }

      this.sessions.clear();
      this.meta = null;

      await this.saveMeta();

      return new Response('ok');
    }

    /*
     * ==================================
     * ROOM INITIALIZATION
     * ==================================
     */
    if (
      url.pathname === '/init' &&
      request.method === 'POST'
    ) {
      if (
        request.headers.get(
          'X-MAGD-Internal',
        ) !== '1'
      ) {
        return new Response(
          'Forbidden',
          { status: 403 },
        );
      }

      /*
       * Never overwrite an existing room.
       */
      if (this.meta) {
        return new Response(
          JSON.stringify({
            error:
              'Room already exists',
          }),
          {
            status: 409,
            headers: {
              'Content-Type':
                'application/json',
            },
          },
        );
      }

      const body =
        await request
          .json()
          .catch(() => null) as
          | Record<string, unknown>
          | null;

      const code =
        text(
          body?.code,
          '',
          64,
        );

      const hostSubject =
        text(
          body?.hostSubject,
          '',
          128,
        );

      if (
        !code ||
        !hostSubject
      ) {
        return new Response(
          JSON.stringify({
            error:
              'Missing room code or host subject',
          }),
          {
            status: 400,
            headers: {
              'Content-Type':
                'application/json',
            },
          },
        );
      }

      const password =
        typeof body?.password === 'string'
          ? body.password
          : '';

      if (
        password.length >
        MAX_PASSWORD
      ) {
        return new Response(
          JSON.stringify({
            error:
              'Password too long',
          }),
          {
            status: 400,
            headers: {
              'Content-Type':
                'application/json',
            },
          },
        );
      }

      this.meta = {
        code,

        name: text(
          body?.name,
          'MAGD Server',
          MAX_ROOM_NAME,
        ),

        map: text(
          body?.map,
          'crossfire',
          MAX_MAP_NAME,
        ),

        game: text(
          body?.game,
          'valve',
          MAX_GAME_NAME,
        ),

        hostName: text(
          body?.hostName,
          'Host',
          MAX_HOST_NAME,
        ),

        /*
         * Room is created before the
         * host WebSocket connects.
         */
        players: 0,

        maxPlayers:
          clampMaxPlayers(
            body?.maxPlayers,
          ),

        hasPassword:
          password.length > 0,

        passwordHash:
          password
            ? await hashPassword(
                password,
              )
            : undefined,

        hostSubject,

        createdAt:
          Date.now(),

        lastHeartbeat:
          Date.now(),
      };

      await this.saveMeta();

      return new Response(
        JSON.stringify(
          this.safeMeta(),
        ),
        {
          status: 201,
          headers: {
            'Content-Type':
              'application/json',
          },
        },
      );
    }

    /*
     * ==================================
     * ROOM INFO
     * ==================================
     */
    if (
      url.pathname === '/info' &&
      request.method === 'GET'
    ) {
      if (!this.meta) {
        return new Response(
          JSON.stringify({
            error:
              'Room not found',
          }),
          {
            status: 404,
            headers: {
              'Content-Type':
                'application/json',
            },
          },
        );
      }

      /*
       * Recalculate from actual attached
       * WebSockets after hibernation.
       */
      this.syncPlayerCount();

      return new Response(
        JSON.stringify(
          this.safeMeta(),
        ),
        {
          headers: {
            'Content-Type':
              'application/json',
          },
        },
      );
    }

    /*
     * ==================================
     * WEBSOCKET CONNECTION
     * ==================================
     */
    if (
      url.pathname === '/ws' &&
      request.method === 'GET'
    ) {
      if (
        !isWebSocketUpgrade(
          request,
        )
      ) {
        return new Response(
          'Expected WebSocket',
          { status: 400 },
        );
      }

      if (!this.meta) {
        return new Response(
          'Room not found',
          { status: 404 },
        );
      }

      const subject =
        request.headers.get(
          'X-MAGD-Subject',
        );

      const role =
        request.headers.get(
          'X-MAGD-Role',
        );

      const password =
        request.headers.get(
          'X-MAGD-Room-Password',
        ) ?? '';

      if (
        !subject ||
        (
          role !== 'host' &&
          role !== 'client'
        )
      ) {
        return new Response(
          'Unauthorized',
          { status: 401 },
        );
      }

      const host =
        role === 'host';

      /*
       * HOST
       */
      if (host) {
        /*
         * Only the exact host subject
         * belonging to this room can host.
         */
        if (
          subject !==
          this.meta.hostSubject
        ) {
          return new Response(
            'Forbidden',
            { status: 403 },
          );
        }

        /*
         * One host socket only.
         */
        if (this.findHost()) {
          return new Response(
            'Host already connected',
            { status: 409 },
          );
        }
      }

      /*
       * CLIENT
       */
      else {
        /*
         * The host counts toward maxPlayers.
         */
        if (
          this.sessions.size >=
          this.meta.maxPlayers
        ) {
          return new Response(
            'Room full',
            { status: 409 },
          );
        }

        /*
         * Password-protected room.
         */
        if (
          this.meta.hasPassword
        ) {
          if (
            !password ||
            !this.meta.passwordHash ||
            await hashPassword(
              password,
            ) !==
              this.meta.passwordHash
          ) {
            return new Response(
              'Forbidden',
              { status: 403 },
            );
          }
        }

        /*
         * Prevent the same token identity
         * from occupying multiple slots.
         */
        if (
          [...this.sessions.values()]
            .some(
              (session) =>
                session.subject ===
                subject,
            )
        ) {
          return new Response(
            'Client already connected',
            { status: 409 },
          );
        }
      }

      /*
       * Assign virtual peer ID.
       */
      const peerId =
        host
          ? 0
          : this.findPeerId();

      if (
        !host &&
        peerId < 1
      ) {
        return new Response(
          'No peer slot available',
          { status: 409 },
        );
      }

      /*
       * Create actual WebSocket pair.
       */
      const pair =
        new WebSocketPair();

      const [
        client,
        server,
      ] = Object.values(pair);

      const attachment:
        SessionAttachment = {
        version: 1,

        subject,

        role:
          host
            ? 'host'
            : 'client',

        peerId,
      };

      /*
       * IMPORTANT:
       * acceptWebSocket() puts the
       * connection into Hibernation API.
       */
      this.ctx.acceptWebSocket(
        server,
      );

      /*
       * IMPORTANT:
       * Persist identity across
       * Durable Object hibernation.
       */
      server.serializeAttachment(
        attachment,
      );

      this.sessions.set(
        server,
        {
          ...attachment,
          socket: server,
        },
      );

      this.syncPlayerCount();

      this.meta.lastHeartbeat =
        Date.now();

      await this.saveMeta();

      /*
       * Send WELCOME.
       */
      try {
        server.send(
          createMessage(
            MagdMessageType.WELCOME,
            new TextEncoder().encode(
              JSON.stringify({
                clientId:
                  subject,

                peerId,

                isHost:
                  host,

                code:
                  this.meta.code,
              }),
            ),
          ),
        );
      } catch {
        this.sessions.delete(
          server,
        );

        this.syncPlayerCount();

        await this.saveMeta();

        try {
          server.close(
            1011,
            'Welcome failed',
          );
        } catch {}

        return new Response(
          'WebSocket initialization failed',
          { status: 500 },
        );
      }

      return new Response(
        null,
        {
          status: 101,
          webSocket: client,
        },
      );
    }

    return new Response(
      'Not found',
      { status: 404 },
    );
  }

  /*
   * ==================================
   * WEBSOCKET MESSAGE
   * ==================================
   */
  async webSocketMessage(
    ws: WebSocket,
    message:
      | ArrayBuffer
      | string,
  ): Promise<void> {
    /*
     * Xash/MAGD uses binary frames.
     */
    if (
      typeof message === 'string'
    ) {
      return;
    }

    await this.handleMessage(
      ws,
      message,
    );
  }

  /*
   * ==================================
   * WEBSOCKET CLOSE
   * ==================================
   */
  async webSocketClose(
    ws: WebSocket,
  ): Promise<void> {
    const session =
      this.sessions.get(ws);

    if (!session) {
      return;
    }

    this.sessions.delete(ws);

    if (!this.meta) {
      return;
    }

    /*
     * If host leaves, the room ends.
     */
    if (
      session.role === 'host'
    ) {
      for (
        const peer
        of this.sessions.values()
      ) {
        try {
          peer.socket.close(
            1001,
            'Host disconnected',
          );
        } catch {}
      }

      this.sessions.clear();

      this.meta = null;

      await this.saveMeta();

      return;
    }

    /*
     * Client left.
     */
    this.syncPlayerCount();

    this.meta.lastHeartbeat =
      Date.now();

    await this.saveMeta();
  }

  /*
   * ==================================
   * WEBSOCKET ERROR
   * ==================================
   */
  async webSocketError(
    ws: WebSocket,
  ): Promise<void> {
    try {
      ws.close(
        1011,
        'WebSocket error',
      );
    } catch {}
  }
}