// Opt-in wall-clock coverage of the harness + real native runtime. No desktop/provider account.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {spawn} from 'node:child_process';
import readline from 'node:readline';
import {mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {OpenAiHarness, HarnessPreferences} from '../dist/openai.js';
import {engineEnvironment} from '../dist/chat.js';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const duration = Number(process.env.RELAY_SUSTAINED_TEST_MS ?? 0);
test('sustained native editing/captures survive two service disconnects without duplicate creation', {skip: duration < 1000, timeout: duration + 20000}, async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-sustained-'));
  mkdirSync(path.join(directory, 'protocol')); writeFileSync(path.join(directory, 'protocol/relay.protocol.json'), readFileSync(path.join(root, 'protocol/relay.protocol.json')));
  const child = spawn(process.env.RELAY_SESSION_TEST_HOST ?? path.join(root, 'build/dev/relay_session_test_host'), [], {cwd: directory, env: engineEnvironment(process.env, 'f'.repeat(64)), stdio: ['pipe', 'pipe', 'pipe']});
  child.stderr.resume(); const pending = new Map(); let serial = 0;
  readline.createInterface({input: child.stdout}).on('line', line => {
    const value = JSON.parse(line), callback = pending.get(value.id);
    if (callback) {pending.delete(value.id); value.ok ? callback.resolve(value.result) : callback.reject(new Error(value.error));}
  });
  const invoke = (method, parameters = {}, host = false) => new Promise((resolve, reject) => {
    const id = ++serial; pending.set(id, {resolve, reject}); child.stdin.write(JSON.stringify({host, request: {id, method, ...parameters}}) + '\n');
  });
  const started = Date.now(); let entity, calls = 0, creations = 0, captures = 0, factories = 0, timer;
  const services = [];
  class Service {
    onMessage = () => {}; disconnected = false;
    async request(method, params) {
      if (method === 'account/read') return {account: {type: 'chatgpt', email: 'fixture@example.invalid'}};
      if (method === 'model/list') return {data: [{id: 'fixture', model: 'fixture', displayName: 'Fixture', isDefault: true, defaultReasoningEffort: 'low', supportedReasoningEfforts: [{reasoningEffort: 'low', description: 'Fixture'}]}]};
      if (method === 'thread/start' || method === 'thread/resume') return {thread: {id: 'sustained-thread'}};
      if (method === 'thread/read') return {thread: {status: {type: 'active'}}};
      if (method === 'turn/start') {setImmediate(() => this.next()); return {turn: {id: 'sustained-turn'}};}
      if (method === 'turn/interrupt') this.emit('turn/completed', {turn: {id: 'sustained-turn', status: 'interrupted'}});
      return {};
    }
    emit(method, params = {}, id) {this.onMessage(method, {threadId: 'sustained-thread', turnId: 'sustained-turn', ...params}, id);}
    next() {
      if (this.disconnected) return;
      if (Date.now() - started >= duration) {this.emit('turn/completed', {turn: {id: 'sustained-turn', status: 'completed'}}); return;}
      const id = ++calls;
      const tool = !entity ? 'scene_create' : id % 12 === 0 ? 'render_capture' : 'scene_set_transform';
      const arguments_ = !entity ? {name: 'Sustained object'} : tool === 'render_capture' ? {path: 'sustained.png', source: 'deterministic'} : {entity, px: id / 10};
      this.emit('item/tool/call', {callId: `call-${id}`, tool, arguments: arguments_}, id);
    }
    reply(id, result) {
      assert.equal(result.success, true);
      const value = JSON.parse(result.contentItems[0].text);
      if (value.entity && !entity) {entity = value.entity; ++creations;}
      if (value.path) {++captures; assert.equal(result.contentItems[1].type, 'inputImage');}
      if ((factories === 1 && calls >= 20) || (factories === 2 && calls >= 60)) {
        this.disconnected = true; this.emit('relay/disconnected'); return;
      }
      timer = setTimeout(() => this.next(), 250);
    }
    close() {this.disconnected = true;}
  }
  const harness = new OpenAiHarness(directory, invoke, new HarnessPreferences(directory), () => {++factories; const service = new Service(); services.push(service); return service;}, 1000);
  try {
    await invoke('session.auto_approval', {enabled: true}, true);
    await harness.submit('Keep adjusting the fixture object and capturing until verified.', async () => {});
    assert.equal(harness.view().status, 'Ready'); assert.equal(creations, 1); assert.equal(factories, 3);
    assert.ok(calls > 60); assert.ok(captures > 4); assert.ok(Date.now() - started >= duration);
    assert.equal((await invoke('scene.list')).entities.length, 1);
    assert.equal(pending.size, 0);
    assert.ok(Buffer.byteLength(JSON.stringify(harness.view())) < 58000);
    console.log(`Sustained fixture: ${Date.now() - started} ms, ${calls} native tool calls, ${captures} PNG image replies, two recoveries, one entity.`);
  } finally {
    clearTimeout(timer); harness.close(); services.forEach(service => service.close()); child.stdin.end(); child.kill();
    if (child.exitCode === null) await new Promise(resolve => child.once('exit', resolve));
    rmSync(directory, {recursive: true, force: true});
  }
});
