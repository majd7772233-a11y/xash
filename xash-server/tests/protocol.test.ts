import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  GAME_BROADCAST,
  GAME_HAS_SENDER,
  MAGD_MAGIC,
  MagdMessageType,
  createGameDatagram,
  createMessage,
  parseGameDatagram,
  parseHeader,
} from '../src/protocol.ts';

test('MAGD header round-trip', () => {
  const frame = createMessage(
    MagdMessageType.PING,
    new Uint8Array([1, 2, 3]),
  );

  const header = parseHeader(frame.buffer);

  assert.ok(header);
  assert.equal(header.magic, MAGD_MAGIC);
  assert.equal(header.type, MagdMessageType.PING);
  assert.equal(header.length, 3);
});

test('truncated MAGD frame is rejected', () => {
  const frame = createMessage(
    MagdMessageType.PING,
    new Uint8Array([1, 2, 3]),
  );

  assert.equal(
    parseHeader(frame.buffer.slice(0, -1)),
    null,
  );
});

test('game datagram preserves flags, peer and payload', () => {
  const frame = createGameDatagram(
    GAME_HAS_SENDER,
    7,
    new Uint8Array([9, 8, 7, 6]),
  );

  const packet = parseGameDatagram(frame.buffer);

  assert.ok(packet);
  assert.equal(packet.flags, GAME_HAS_SENDER);
  assert.equal(packet.peerId, 7);
  assert.deepEqual(
    [...packet.data],
    [9, 8, 7, 6],
  );
});

test('broadcast peer id is accepted', () => {
  const frame = createGameDatagram(
    0,
    GAME_BROADCAST,
    new Uint8Array([1]),
  );

  const packet = parseGameDatagram(frame.buffer);

  assert.ok(packet);
  assert.equal(packet.peerId, GAME_BROADCAST);
});

test('trailing bytes are rejected', () => {
  const frame = createMessage(
    MagdMessageType.PING,
    new Uint8Array([1, 2]),
  );

  const padded = new Uint8Array(frame.byteLength + 1);
  padded.set(frame);
  padded[padded.length - 1] = 99;

  assert.equal(
    parseHeader(padded.buffer),
    null,
  );
});

test('GAME_DATAGRAM larger than Xash limit is rejected', () => {
  const tooLarge = new Uint8Array(16_385);

  assert.throws(
    () =>
      createGameDatagram(
        0,
        1,
        tooLarge,
      ),
    RangeError,
  );
});
