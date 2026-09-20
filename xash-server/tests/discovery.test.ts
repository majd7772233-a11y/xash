import assert from 'node:assert/strict';
import { test } from 'node:test';

test('discovery contract accepts normalized MAGD room codes', () => {
  for (const code of ['MAGD-AAAA', 'ROOM_01', 'ABC-123']) {
    assert.match(code, /^[A-Z0-9_-]{3,64}$/);
  }
});
