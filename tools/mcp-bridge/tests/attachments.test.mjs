import assert from 'node:assert/strict';
import { test } from 'node:test';
import { mkdtempSync, writeFileSync, rmSync, symlinkSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { Attachments } from '../dist/attachments.js';
test('human attachments provide image input, bounded reads and no arbitrary path access', () => {
  const root = mkdtempSync(path.join(os.tmpdir(), 'relay-attachments-'));
  try {
    const text = path.join(root, 'notes.txt'), image = path.join(root, 'image.png'), binary = path.join(root, 'file.bin');
    writeFileSync(text, 'Untrusted instructions\n'.repeat(10000)); writeFileSync(image, Buffer.from([137,80,78,71])); writeFileSync(binary, Buffer.from([0,1,2,3]));
    const store = new Attachments(), prepared = store.prepare(root, [text, image, binary]);
    assert.match(prepared.display, /!\[image.png\]\(\.relay\/chat-files\//);
    assert.equal(prepared.images.length, 1); assert.match(prepared.images[0].url, /^data:image\/png;base64,/);
    assert.match(prepared.text, /untrusted/i); assert.equal(store.files.size, 3);
    const id = [...store.files.keys()][0];
    const chunk = store.read({ id, offset: 10, length: 32768 }); assert.equal(chunk.nextOffset, 32778); assert.equal(chunk.totalBytes, 230000);
    assert.throws(() => store.read({ id: '/etc/passwd' }), /not attached/);
    assert.throws(() => store.read({ id, length: 32769 }), /Invalid/);
    symlinkSync(text, path.join(root, 'symlink.txt')); assert.throws(() => store.prepare(root, [path.join(root, 'symlink.txt')]));
    assert.throws(() => store.prepare(root, Array(9).fill(text)), /eight/);
  } finally { rmSync(root, { recursive: true, force: true }); }
});

import { ChatWorkflow } from '../dist/chat.js';
import { readFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
test('compatible provider receives image parts and attachment failures leave chat usable', async () => {
  const project = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
  const root = mkdtempSync(path.join(os.tmpdir(), 'relay-compatible-attachments-'));
  const previous = globalThis.fetch;
  try {
    mkdirSync(path.join(root, 'protocol')); writeFileSync(path.join(root, 'protocol/relay.protocol.json'), readFileSync(path.join(project, 'protocol/relay.protocol.json')));
    const image = path.join(root, 'image.png'); writeFileSync(image, readFileSync(path.join(project, 'tests/fixtures/chat-media/image.png')));
    let request;
    globalThis.fetch = async (_url, options) => { request = JSON.parse(options.body); return new Response(JSON.stringify({choices: [{message: {role: 'assistant', content: 'Seen'}}]})); };
    const chat = new ChatWorkflow(root, async () => ({auto_approval: true}), {endpoint: 'http://127.0.0.1:1', model: 'fixture', apiKey: ''});
    await chat.submit('', async () => {}, [path.join(root, 'missing.txt')]); assert.equal(chat.view().busy, false);
    await chat.submit('', async () => {}, [image]);
    const content = request.messages.at(-1).content;
    assert.equal(content[1].type, 'image_url'); assert.match(content[1].image_url.url, /^data:image\/png;base64,/);
    assert.equal(JSON.stringify(chat.view()).includes('data:image'), false);
  } finally { globalThis.fetch = previous; rmSync(root, {recursive: true, force: true}); }
});
