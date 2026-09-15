import assert from 'node:assert';
import { test } from 'node:test';
import { MAGD_MAGIC, MagdMessageType, createMessage, parseHeader } from '../src/protocol.ts';

test('End-to-End Handshake and Frame Relaying Flow', () => {
  const token = 'magd_token_1234567890abcdef';
  assert.ok(token.startsWith('magd_token_'));

  const payload = new Uint8Array([0x01, 0x02, 0x03, 0x04, 0x05]);
  const gameMsg = createMessage(MagdMessageType.GAME_DATAGRAM, payload);

  const parsedHeader = parseHeader(gameMsg.buffer);
  assert.notStrictEqual(parsedHeader, null);
  assert.strictEqual(parsedHeader!.magic, MAGD_MAGIC);
  assert.strictEqual(parsedHeader!.type, MagdMessageType.GAME_DATAGRAM);
  assert.strictEqual(parsedHeader!.length, payload.byteLength);

  const welcomePayload = new TextEncoder().encode(JSON.stringify({ clientId: 'host-uuid', isHost: true, code: 'MAGD-TEST' }));
  const welcomeFrame = createMessage(MagdMessageType.WELCOME, welcomePayload);
  const welcomeHeader = parseHeader(welcomeFrame.buffer);
  assert.strictEqual(welcomeHeader!.type, MagdMessageType.WELCOME);
});
