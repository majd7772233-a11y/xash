export const MAGD_MAGIC = 0x4d47;
export const MAGD_HEADER_SIZE = 5;
export const MAGD_GAME_ENVELOPE_SIZE = 2;
export const MAGD_MAX_PACKET_SIZE = 16_384;
export const MAGD_MAX_MESSAGE_SIZE =
  MAGD_HEADER_SIZE + MAGD_GAME_ENVELOPE_SIZE + MAGD_MAX_PACKET_SIZE;

export const MagdMessageType = {
  HELLO: 0x01,
  WELCOME: 0x02,
  PING: 0x03,
  PONG: 0x04,
  HOST_REGISTER: 0x10,
  HOST_UPDATE: 0x11,
  JOIN_ROOM: 0x20,
  READY: 0x21,
  GAME_DATAGRAM: 0x30,
  ERROR: 0xe0,
} as const;

export type MagdMessageTypeValue =
  (typeof MagdMessageType)[keyof typeof MagdMessageType];

export const GAME_HAS_SENDER = 0x01;
export const GAME_HAS_TARGET = 0x02;
export const GAME_BROADCAST = 0xff;

export interface MagdHeader {
  magic: number;
  type: MagdMessageTypeValue;
  length: number;
}

export interface GameDatagram {
  flags: number;
  peerId: number;
  data: Uint8Array;
}

export function createMessage(
  type: MagdMessageTypeValue,
  payload: Uint8Array,
): Uint8Array {
  if (payload.byteLength > 0xffff) {
    throw new RangeError('MAGD payload is too large');
  }

  const packet = new Uint8Array(MAGD_HEADER_SIZE + payload.byteLength);
  const view = new DataView(packet.buffer);
  view.setUint16(0, MAGD_MAGIC, false);
  view.setUint8(2, type);
  view.setUint16(3, payload.byteLength, false);
  packet.set(payload, MAGD_HEADER_SIZE);
  return packet;
}

export function parseHeader(buffer: ArrayBuffer): MagdHeader | null {
  if (buffer.byteLength < MAGD_HEADER_SIZE) return null;
  const view = new DataView(buffer);
  const magic = view.getUint16(0, false);
  const type = view.getUint8(2) as MagdMessageTypeValue;
  const length = view.getUint16(3, false);

  if (magic !== MAGD_MAGIC || buffer.byteLength !== MAGD_HEADER_SIZE + length) {
    return null;
  }

  if (type === MagdMessageType.GAME_DATAGRAM &&
      (length < MAGD_GAME_ENVELOPE_SIZE ||
       length > MAGD_GAME_ENVELOPE_SIZE + MAGD_MAX_PACKET_SIZE)) {
    return null;
  }

  return { magic, type, length };
}

export function createGameDatagram(
  flags: number,
  peerId: number,
  data: Uint8Array,
): Uint8Array {
  if (data.byteLength > MAGD_MAX_PACKET_SIZE) {
    throw new RangeError('Game datagram is too large');
  }
  if (!Number.isInteger(flags) || flags < 0 || flags > 0xff) {
    throw new RangeError('Invalid MAGD flags');
  }
  if (!Number.isInteger(peerId) || peerId < 0 || peerId > 0xff) {
    throw new RangeError('Invalid MAGD peer id');
  }

  const payload = new Uint8Array(MAGD_GAME_ENVELOPE_SIZE + data.byteLength);
  payload[0] = flags;
  payload[1] = peerId;
  payload.set(data, MAGD_GAME_ENVELOPE_SIZE);
  return createMessage(MagdMessageType.GAME_DATAGRAM, payload);
}

export function parseGameDatagram(buffer: ArrayBuffer): GameDatagram | null {
  const header = parseHeader(buffer);
  if (!header || header.type !== MagdMessageType.GAME_DATAGRAM ||
      header.length < MAGD_GAME_ENVELOPE_SIZE) {
    return null;
  }

  const payload = new Uint8Array(buffer, MAGD_HEADER_SIZE, header.length);
  return {
    flags: payload[0],
    peerId: payload[1],
    data: payload.slice(MAGD_GAME_ENVELOPE_SIZE),
  };
}
