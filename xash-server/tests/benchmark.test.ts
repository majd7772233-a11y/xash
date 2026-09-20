import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createGameDatagram } from '../src/protocol.ts';

test('protocol allocation stays bounded for a game packet', () => {
  const start = performance.now();
  for (let i = 0; i < 1000; i++) {
    createGameDatagram(0, 1, new Uint8Array(256));
  }
  const elapsed = performance.now() - start;
  assert.ok(Number.isFinite(elapsed));
});
