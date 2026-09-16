import assert from 'node:assert';
import { test } from 'node:test';
import { generateToken, verifyToken } from '../src/auth.ts';

test('HMAC Token Generation and Expiration Verification', async () => {
  const exp = Date.now() + 60000;
  const token = await generateToken({ sub: 'user_123', exp });

  assert.ok(token.startsWith('magd_token_'));

  const verified = await verifyToken(token);
  assert.notStrictEqual(verified, null);
  assert.strictEqual(verified!.sub, 'user_123');

  // Expired token test
  const expiredToken = await generateToken({ sub: 'user_123', exp: Date.now() - 1000 });
  const verifiedExpired = await verifyToken(expiredToken);
  assert.strictEqual(verifiedExpired, null);
});
