import assert from 'node:assert';
import { test } from 'node:test';
import { hashPassword } from '../src/auth.ts';

test('SHA-256 Room Password Hashing & Verification', async () => {
  const pass1 = 'secret_room_pass';
  const pass2 = 'secret_room_pass';
  const wrongPass = 'wrong_pass';

  const hash1 = await hashPassword(pass1);
  const hash2 = await hashPassword(pass2);
  const hashWrong = await hashPassword(wrongPass);

  assert.strictEqual(hash1, hash2);
  assert.notStrictEqual(hash1, hashWrong);
  assert.strictEqual(hash1.length, 64); // SHA-256 hex string length
});
