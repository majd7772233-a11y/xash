import assert from 'node:assert'; import {test} from 'node:test'; import {createGameDatagram,parseGameDatagram} from '../src/protocol.ts';
test('peer id preserved',()=>{const f=createGameDatagram(1,12,new Uint8Array([5,4,3,2,1]));const p=parseGameDatagram(f.buffer);assert.ok(p);assert.equal(p?.peerId,12);});
