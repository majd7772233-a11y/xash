import assert from 'node:assert/strict';
import { test } from 'node:test';
import { generateToken, verifyToken } from '../src/auth.ts';

test('token is bound to the room', async () => {
  const token = await generateToken({
    sub: 'host-1',
    exp: Date.now() + 60_000,
    role: 'host',
    room: 'MAGD-AAAA',
  }, 'secret');
  const payload = await verifyToken(token, 'secret');
  assert.equal(payload?.room, 'MAGD-AAAA');
  assert.equal(payload?.role, 'host');
});

test('wrong secret fails', async () => {
  const token = await generateToken({
    sub: 'client-1',
    exp: Date.now() + 60_000,
    role: 'client',
    room: 'MAGD-AAAA',
  }, 'a');
  assert.equal(await verifyToken(token, 'b'), null);
});

test('expired token fails', async () => {
  const token = await generateToken({
    sub: 'client-1',
    exp: Date.now() - 1,
    role: 'client',
    room: 'MAGD-AAAA',
  }, 'secret').catch(() => null);
  assert.equal(token, null);
});
