import assert from 'node:assert'; import {test} from 'node:test'; import {hashPassword} from '../src/auth.ts';
test('password hash',async()=>{const a=await hashPassword('x'),b=await hashPassword('x'),c=await hashPassword('y');assert.equal(a,b);assert.notEqual(a,c);});
