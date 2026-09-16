import assert from 'node:assert';
import { test } from 'node:test';
import { MagdMessageType, createMessage, parseHeader } from '../src/protocol.ts';

test('Internet Client Join Datagram Relaying', () => {
  const datagramPayload = new Uint8Array([0x05, 0x04, 0x03, 0x02, 0x01]);
  const frame = createMessage(MagdMessageType.GAME_DATAGRAM, datagramPayload);

  const header = parseHeader(frame.buffer);
  assert.notStrictEqual(header, null);
  assert.strictEqual(header!.type, MagdMessageType.GAME_DATAGRAM);
  assert.strictEqual(header!.length, datagramPayload.byteLength);
});
