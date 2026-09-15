import assert from 'node:assert';
import { test } from 'node:test';
import { MAGD_MAGIC, MagdMessageType, createMessage, parseHeader } from '../src/protocol.ts';

test('MAGD Protocol Header Parsing and Creation', () => {
  const payload = new TextEncoder().encode('Test Payload');
  const message = createMessage(MagdMessageType.GAME_DATAGRAM, payload);

  assert.strictEqual(message.byteLength, 5 + payload.byteLength);

  const header = parseHeader(message.buffer);
  assert.notStrictEqual(header, null);
  assert.strictEqual(header!.magic, MAGD_MAGIC);
  assert.strictEqual(header!.type, MagdMessageType.GAME_DATAGRAM);
  assert.strictEqual(header!.length, payload.byteLength);
});

test('MAGD Protocol Invalid Headers Rejection', () => {
  // Too short buffer
  const shortBuffer = new Uint8Array([0x4d, 0x47, 0x01]).buffer;
  assert.strictEqual(parseHeader(shortBuffer), null);

  // Invalid magic bytes
  const invalidMagic = new Uint8Array([0x00, 0x00, 0x01, 0x00, 0x00]).buffer;
  assert.strictEqual(parseHeader(invalidMagic), null);
});
