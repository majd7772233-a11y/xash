import assert from 'node:assert/strict';
import { test } from 'node:test';
import { GAME_HAS_SENDER, createGameDatagram, parseGameDatagram } from '../src/protocol.ts';

test('joined client gets a routable sender identity', () => {
  const frame = createGameDatagram(GAME_HAS_SENDER, 1, new Uint8Array([42]));
  const parsed = parseGameDatagram(frame.buffer);
  assert.ok(parsed);
  assert.equal(parsed.peerId, 1);
  assert.equal(parsed.flags & GAME_HAS_SENDER, GAME_HAS_SENDER);
});
