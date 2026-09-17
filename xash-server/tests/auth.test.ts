import assert from 'node:assert'; import {test} from 'node:test'; import {generateToken,verifyToken} from '../src/auth.ts';
test('token room scope',async()=>{const t=await generateToken({sub:'h',exp:Date.now()+10000,role:'host',room:'MAGD-AAAA'},'s');const p=await verifyToken(t,'s');assert.ok(p);assert.equal(p?.room,'MAGD-AAAA');});
test('wrong secret fails',async()=>{const t=await generateToken({sub:'c',exp:Date.now()+10000,role:'client'},'a');assert.equal(await verifyToken(t,'b'),null);});
test('expired fails',async()=>{const t=await generateToken({sub:'c',exp:Date.now()-1,role:'client'},'s');assert.equal(await verifyToken(t,'s'),null);});
