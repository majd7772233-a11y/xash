import assert from 'node:assert';
import { test } from 'node:test';

test('Room Discovery Structure', () => {
  const room = {
    code: 'MAGD-1234',
    name: "Majd's Server",
    map: 'crossfire',
    game: 'valve',
    hostName: 'Majd',
    players: 4,
    maxPlayers: 8,
    hasPassword: false,
    createdAt: Date.now(),
    lastHeartbeat: Date.now()
  };

  assert.strictEqual(room.code, 'MAGD-1234');
  assert.strictEqual(room.players, 4);
  assert.strictEqual(room.maxPlayers, 8);
});
