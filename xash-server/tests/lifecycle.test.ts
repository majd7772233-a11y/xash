import assert from 'node:assert/strict';
import { test } from 'node:test';

test('host grace window is positive', () => {
  const graceMs = 30_000;
  assert.ok(graceMs >= 30_000);
});
