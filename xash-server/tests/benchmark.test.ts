import assert from 'node:assert'; import {test} from 'node:test'; import {createGameDatagram,parseGameDatagram} from '../src/protocol.ts';
test('framing smoke',()=>{const p=new Uint8Array(1200);for(let i=0;i<1000;i++)assert.ok(parseGameDatagram(createGameDatagram(0,0,p).buffer));});
