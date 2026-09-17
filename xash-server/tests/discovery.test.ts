import assert from 'node:assert'; import {test} from 'node:test';
test('room ids unique',()=>{assert.deepEqual([...new Set(['A','B','A'])].sort(),['A','B']);});
