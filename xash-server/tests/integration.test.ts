import assert from 'node:assert/strict';
import { test } from 'node:test';
import { GAME_HAS_SENDER, createGameDatagram, parseGameDatagram } from '../src/protocol.ts';

test('client-to-host relay contract', () => {
  const frame = createGameDatagram(GAME_HAS_SENDER, 4, new Uint8Array([1, 2, 3]));
  const packet = parseGameDatagram(frame.buffer);
  assert.ok(packet);
  assert.equal(packet.peerId, 4);
  assert.deepEqual([...packet.data], [1, 2, 3]);
});
