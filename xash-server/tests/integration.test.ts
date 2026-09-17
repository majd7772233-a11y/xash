import assert from 'node:assert'; import {test} from 'node:test'; import {createGameDatagram,parseGameDatagram,GAME_HAS_SENDER} from '../src/protocol.ts';
test('client to host contract',()=>{const f=createGameDatagram(GAME_HAS_SENDER,4,new Uint8Array([1,2,3]));const p=parseGameDatagram(f.buffer);assert.ok(p);assert.equal(p?.peerId,4);assert.deepEqual([...p!.data],[1,2,3]);});
