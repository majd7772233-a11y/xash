export const MAGD_MAGIC = 0x4d47; // 'M' 'G'

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
  ERROR: 0xe0
} as const;

export type MagdMessageTypeValue = typeof MagdMessageType[keyof typeof MagdMessageType];

export const MagdSessionState = {
  DISCONNECTED: 0,
  CONNECTING: 1,
  AUTHENTICATED: 2,
  IN_ROOM: 3,
  READY: 4,
  ERROR: 5
} as const;

export type MagdSessionStateValue = typeof MagdSessionState[keyof typeof MagdSessionState];

export interface MagdHeader {
  magic: number;
  type: MagdMessageTypeValue;
  length: number;
}

export function parseHeader(buffer: ArrayBuffer): MagdHeader | null {
  if (buffer.byteLength < 5) return null;
  const view = new DataView(buffer);
  const magic = view.getUint16(0, false);
  if (magic !== MAGD_MAGIC) return null;
  const type = view.getUint8(2) as MagdMessageTypeValue;
  const length = view.getUint16(3, false);
  if (buffer.byteLength < 5 + length) return null;
  return { magic, type, length };
}

export function createMessage(type: MagdMessageTypeValue, payload: Uint8Array): Uint8Array {
  const packet = new Uint8Array(5 + payload.byteLength);
  const view = new DataView(packet.buffer);
  view.setUint16(0, MAGD_MAGIC, false);
  view.setUint8(2, type);
  view.setUint16(3, payload.byteLength, false);
  packet.set(payload, 5);
  return packet;
}
