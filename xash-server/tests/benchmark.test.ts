import assert from 'node:assert';
import { test } from 'node:test';
import { MagdMessageType, createMessage, parseHeader } from '../src/protocol.ts';

test('Transport Performance Benchmark (10,000 Datagrams)', () => {
  const payload = new Uint8Array(1024); // 1KB game packet simulation
  for (let i = 0; i < payload.length; i++) {
    payload[i] = i & 0xff;
  }

  const iterations = 10000;
  const startTime = performance.now();

  for (let i = 0; i < iterations; i++) {
    const msg = createMessage(MagdMessageType.GAME_DATAGRAM, payload);
    const header = parseHeader(msg.buffer);
    assert.strictEqual(header!.type, MagdMessageType.GAME_DATAGRAM);
  }

  const durationMs = performance.now() - startTime;
  const opsPerSec = (iterations / (durationMs / 1000)).toFixed(0);

  assert.ok(durationMs < 2000, `Benchmark took ${durationMs}ms`);
});
