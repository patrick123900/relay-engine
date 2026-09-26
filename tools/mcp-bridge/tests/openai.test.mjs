import assert from 'node:assert/strict';
import { test } from 'node:test';
import { spawn, spawnSync } from 'node:child_process';
import readline from 'node:readline';
import { mkdtempSync, rmSync, readFileSync, statSync, mkdirSync, writeFileSync, unlinkSync, symlinkSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { captureImage, CodexTransport, OpenAiHarness, HarnessPreferences } from '../dist/openai.js';
import { engineEnvironment, parameterSchema } from '../dist/chat.js';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const until = async predicate => { for (let i = 0; i < 100 && !predicate(); ++i) await new Promise(resolve => setTimeout(resolve, 5)); assert.ok(predicate(), 'expected asynchronous harness state'); };
class Service {
  onMessage = () => {};
  requests = []; replies = [];
  account = null;
  onTurn = () => {};
  onReply = () => {};
  async request(method, params = {}) {
    this.requests.push({method, params});
    if (method === 'account/read') return { account: this.account };
    if (method === 'model/list') return { data: [{ id: 'model-a', model: 'fixture-model', displayName: 'Fixture Model', isDefault: true, defaultReasoningEffort: 'low', supportedReasoningEfforts: [{reasoningEffort: 'low', description: 'Fast'}, {reasoningEffort: 'high', description: 'Careful'}] }], nextCursor: null };
    if (method === 'account/login/start') return { type: 'chatgptDeviceCode', loginId: 'login-fixture', verificationUrl: 'https://auth.openai.com/codex/device', userCode: 'FIXT-1234', accessToken: 'fixture-token-never-project' };
    if (method === 'account/logout') { this.account = null; return {}; }
    if (method === 'thread/start' || method === 'thread/resume') return { thread: { id: 'thread-fixture' } };
    if (method === 'turn/start') { setImmediate(() => this.onTurn()); return { turn: { id: 'turn-fixture' } }; }
    if (method === 'turn/interrupt') { this.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'interrupted'}}); return {}; }
    return {};
  }
  emit(method, params = {}, id) { this.onMessage(method, { ...(method === "item/tool/call" ? {callId: `call-${id}`} : {}), threadId: 'thread-fixture', turnId: 'turn-fixture', ...params }, id); }
  reply(id, result) { this.replies.push({id, result}); this.onReply(id, result); }
  close() {}
  async login(harness) {
    await harness.control({action: 'signin'});
    assert.equal(harness.view().openai.login.userCode, 'FIXT-1234');
    this.account = {type: 'chatgpt', email: 'fixture@example.invalid', planType: 'plus', accessToken: 'fixture-token-never-project'};
    this.emit('account/login/completed', {loginId: 'login-fixture', success: true});
    await until(() => harness.view().openai.account?.type === 'chatgpt');
  }
}

test('device login, account projection, model selection and logout use supported RPC without exposing tokens', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-openai-'));
  const preferences = new HarnessPreferences(directory);
  const service = new Service();
  const harness = new OpenAiHarness(root, async () => ({}), preferences, () => service);
  try {
    await service.login(harness);
    assert.equal(JSON.stringify(harness.view()).includes('fixture-token-never-project'), false);
    assert.equal(service.requests.find(request => request.method === 'account/login/start').params.type, 'chatgptDeviceCode');
    await harness.control({action: 'select', model: 'fixture-model', effort: 'high'});
    assert.equal(harness.view().openai.effort, 'high');
    assert.equal(new HarnessPreferences(directory).value.effort, 'high');
    assert.equal(readFileSync(path.join(directory, '.relay/agent-harness.json'), 'utf8').includes('fixture-token'), false);
    await harness.control({action: 'select', model: 'unavailable', effort: 'high'});
    assert.equal(harness.view().openai.model, 'fixture-model');
    await harness.control({action: 'signin'});
    await harness.control({action: 'cancel_signin'});
    assert.equal(harness.view().openai.login, null);
    assert.ok(service.requests.some(request => request.method === 'account/login/cancel'));
    await harness.control({action: 'signout'});
    assert.equal(harness.view().openai.account, null);
    assert.ok(service.requests.some(request => request.method === 'account/logout'));
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('streaming OpenAI harness dispatches real native tools, blocks host tools and cancels active turns', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-openai-native-'));
  const token = 'e'.repeat(64);
  const child = spawn(process.env.RELAY_SESSION_TEST_HOST ?? path.join(root, 'build/dev/relay_session_test_host'), [], { cwd: directory, env: engineEnvironment({...process.env, RELAY_AGENT_GRANTS: ''}, token), stdio: ['pipe', 'pipe', 'pipe'] });
  child.stderr.resume(); let serial = 0; const pending = new Map();
  readline.createInterface({input: child.stdout}).on('line', line => { const response = JSON.parse(line); const callback = pending.get(response.id); if (!callback) return; pending.delete(response.id); response.ok ? callback.resolve(response.result) : callback.reject(new Error(response.error)); });
  const invoke = (method, parameters = {}, host = false) => new Promise((resolve, reject) => { const id = ++serial; pending.set(id, {resolve, reject}); child.stdin.write(JSON.stringify({host, request: {id, method, ...parameters}}) + '\n'); });
  const preferences = new HarnessPreferences(directory), service = new Service();
  const harness = new OpenAiHarness(root, invoke, preferences, () => service);
  try {
    await service.login(harness);
    await invoke('session.auto_approval', {enabled: true}, true);
    service.onTurn = () => {
      service.emit('item/agentMessage/delta', {itemId: 'message-a', delta: 'Making '});
      service.emit('item/agentMessage/delta', {itemId: 'message-a', delta: 'a light.'});
      service.emit('item/tool/call', {tool: 'scene_create', arguments: {name: 'OpenAI fixture'}}, 100);
    };
    service.onReply = (id, result) => {
      if (id === 100) { assert.equal(result.success, true); service.emit('item/tool/call', {tool: 'session_auto_approval', arguments: {enabled: false}}, 101); }
      if (id === 101) { assert.equal(result.success, false); service.emit('item/completed', {item: {id: 'message-a', type: 'agentMessage', text: 'Created the entity.'}}); service.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'completed'}}); }
    };
    await harness.submit('Create an entity', async () => {});
    assert.equal(harness.view().busy, false);
    assert.equal(harness.view().messages.at(-1).content, 'Created the entity.');
    assert.equal((await invoke('scene.list')).entities.length, 1);
    const start = service.requests.find(request => request.method === 'thread/start').params;
    assert.equal(start.ephemeral, false); assert.equal(start.sandbox, 'read-only');
    assert.ok(start.dynamicTools.some(tool => tool.name === 'scene_create'));
    assert.equal(start.dynamicTools.some(tool => tool.name === 'chat_control' || tool.name === 'session_auto_approval'), false);
    service.onTurn = () => {}; service.onReply = () => {};
    const active = harness.submit('Wait for Stop', async () => {});
    await until(() => service.requests.filter(request => request.method === 'turn/start').length === 2);
    harness.cancel(); await active;
    assert.equal(harness.view().status, 'Stopped'); assert.equal(harness.view().busy, false);
    assert.equal((await invoke('scene.list')).entities.length, 1, 'Stop preserves completed actions');
    await harness.control({action: 'new_chat'});
    assert.equal(harness.view().messages.length, 0);
  } finally { harness.close(); child.stdin.end(); child.kill(); rmSync(directory, {recursive: true, force: true}); }
});

test('pending approval halts subsequent dynamic actions and unsupported device URLs are refused', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-openai-approval-'));
  const service = new Service(); let mutations = 0;
  const harness = new OpenAiHarness(root, async method => {
    if (method === 'session.status') return {auto_approval: false};
    if (method === 'session.request') return {request: 1, pending: true};
    ++mutations; return {};
  }, new HarnessPreferences(directory), () => service);
  try {
    await service.login(harness);
    service.onTurn = () => { service.emit('item/tool/call', {tool: 'session_request', arguments: {scope: 'scene.create'}}, 1); service.emit('item/tool/call', {tool: 'scene_create', arguments: {name: 'Must not execute'}}, 2); };
    await harness.submit('Need approval', async () => {});
    await until(() => service.replies.length === 2);
    assert.equal(mutations, 0);
    assert.equal(service.replies[1].result.success, false);
    assert.match(harness.view().status, /Waiting for access/);
    const original = service.request.bind(service);
    service.request = async (method, params) => method === 'account/login/start' ? {loginId: 'bad', userCode: 'CODE', verificationUrl: 'https://example.invalid/device'} : original(method, params);
    await harness.control({action: 'signin'});
    assert.equal(harness.view().openai.login, null);
    assert.match(harness.view().status, /Unexpected device/);
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});


test('installed Codex runtime accepts isolated metadata and all Relay dynamic tools without model calls', async context => {
  const executable = process.env.RELAY_CODEX_EXECUTABLE ?? 'codex';
  if (spawnSync(executable, ['--version'], {encoding: 'utf8'}).status !== 0) { context.skip('Codex CLI unavailable'); return; }
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-openai-runtime-'));
  const service = new CodexTransport(directory, executable);
  try {
    await service.request('initialize', {clientInfo: {name: 'relay_engine_test', version: '0.1.0'}, capabilities: {experimentalApi: true}}); service.notify('initialized');
    const account = await service.request('account/read', {refreshToken: false});
    assert.equal(account.account, null, 'runtime must not reuse desktop/CLI host credentials');
    const configuration = await service.request('config/read', {includeLayers: false});
    assert.equal(configuration.config.features.code_mode_host, true, 'Relay dynamic tools require the stable tool host at inference time');
    assert.equal(configuration.config.features.code_mode, false);
    assert.equal(configuration.config.features.shell_tool, false);
    assert.equal(configuration.config.features.computer_use, false);
    const catalog = await service.request('model/list', {limit: 100, includeHidden: false});
    assert.ok(catalog.data.length > 0);
    const methods = JSON.parse(readFileSync(path.join(root, 'protocol/relay.protocol.json'), 'utf8')).methods.filter(method => !method.hostOnly && !method.bridgeOnly);
    const dynamicTools = methods.map(method => ({type: 'function', name: method.tool, description: method.description, inputSchema: {type: 'object', additionalProperties: false, properties: Object.fromEntries(method.params.map(parameter => [parameter.wire ?? parameter.name, parameterSchema(parameter)])), required: method.params.filter(parameter => parameter.required).map(parameter => parameter.wire ?? parameter.name)}}));
    const result = await service.request('thread/start', {cwd: path.join(directory, '.relay/openai/workspace'), ephemeral: false, approvalPolicy: 'never', sandbox: 'read-only', dynamicTools, baseInstructions: 'Use only Relay tools.'});
    assert.ok(result.thread.id); assert.equal(result.thread.ephemeral, false);
    assert.equal(statSync(path.join(directory, '.relay/openai')).mode & 0o777, 0o700);
    assert.equal(dynamicTools.length, 132);
  } finally { service.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('OpenAI private home and preference paths reject dangling symlinks', () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-openai-path-'));
  try {
    mkdirSync(path.join(directory, '.relay/openai'), {recursive: true});
    symlinkSync(path.join(directory, 'missing-auth.json'), path.join(directory, '.relay/openai/auth.json'));
    assert.throws(() => new CodexTransport(directory), /unsafe/);
    symlinkSync(path.join(directory, 'missing-settings.json'), path.join(directory, '.relay/agent-harness.json'));
    assert.throws(() => new HarnessPreferences(directory), /unsafe/);
  } finally { rmSync(directory, {recursive: true, force: true}); }
});


test('capture images are returned as bounded PNG content and refuse unsafe files', () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-vision-'));
  const captures = path.join(directory, 'captures'); mkdirSync(captures);
  const bytes = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aAzsAAAAASUVORK5CYII=', 'base64');
  const image = path.join(captures, 'fixture.png'); writeFileSync(image, bytes);
  try {
    const content = captureImage(directory, {path: 'captures/fixture.png', source: 'vulkan'});
    assert.equal(content.type, 'inputImage'); assert.equal(content.imageUrl, 'data:image/png;base64,' + bytes.toString('base64'));
    assert.equal(captureImage(directory, {path: '../fixture.png'}), undefined);
    unlinkSync(image); symlinkSync(path.join(directory, 'outside.png'), image); writeFileSync(path.join(directory, 'outside.png'), bytes);
    assert.equal(captureImage(directory, {path: 'fixture.png'}), undefined);
    unlinkSync(image); writeFileSync(image, 'not a PNG'); assert.equal(captureImage(directory, {path: 'fixture.png'}), undefined);
  } finally { rmSync(directory, {recursive: true, force: true}); }
});


test('capture tool replies attach image content to the model without placing pixels in editor projections', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-vision-tool-'));
  mkdirSync(path.join(directory, 'protocol')); writeFileSync(path.join(directory, 'protocol/relay.protocol.json'), readFileSync(path.join(root, 'protocol/relay.protocol.json')));
  mkdirSync(path.join(directory, 'captures'));
  writeFileSync(path.join(directory, 'captures/fixture.png'), Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aAzsAAAAASUVORK5CYII=', 'base64'));
  const service = new Service();
  const harness = new OpenAiHarness(directory, async method => method === 'session.status' ? {auto_approval: true} : {path: 'captures/fixture.png', source: 'vulkan'}, new HarnessPreferences(directory), () => service);
  try {
    await service.login(harness);
    service.onTurn = () => service.emit('item/tool/call', {tool: 'render_capture', arguments: {path: 'fixture.png', source: 'vulkan'}}, 300);
    service.onReply = (_id, reply) => { assert.equal(reply.success, true); assert.equal(reply.contentItems[1].type, 'inputImage'); service.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'completed'}}); };
    await harness.submit('Capture for visual review', async () => {});
    assert.equal(JSON.stringify(harness.view()).includes('base64'), false);
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});


test('large tool replies reach inference intact while UI summaries remain bounded', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-full-results-'));
  const service = new Service();
  const result = {entities: Array.from({length: 300}, (_, i) => ({entity: `${i}:1`, name: `Entity ${i} 日本語`})), tail: 'last field'};
  const harness = new OpenAiHarness(root, async method => method === 'session.status' ? {auto_approval: true} : result, new HarnessPreferences(directory), () => service);
  try {
    await service.login(harness);
    service.onTurn = () => service.emit('item/tool/call', {tool: 'scene_list', arguments: {}}, 1);
    service.onReply = (_id, reply) => {
      assert.deepEqual(JSON.parse(reply.contentItems[0].text), result);
      assert.ok(reply.contentItems[0].text.length > 4000);
      service.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'completed'}});
    };
    await harness.submit('Inspect a large scene', async () => {});
    assert.ok(harness.view().results[0].summary.length <= 1200);
    assert.ok(Buffer.byteLength(JSON.stringify(harness.view())) < 58000);
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('disconnect during a native mutation resumes the original thread and deduplicates its stable call ID', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-recovery-'));
  const first = new Service(), second = new Service();
  second.account = {type: 'chatgpt', email: 'fixture@example.invalid'};
  let factories = 0, mutations = 0, finishMutation;
  const invoke = async method => {
    if (method === 'session.status') return {auto_approval: true, project: 'fixture-project'};
    if (method === 'scene.list') return {entities: [{entity: '0:1', name: 'Preserved human object'}, {entity: '1:1', name: 'Agent object'}]};
    ++mutations;
    return new Promise(resolve => { finishMutation = () => resolve({entity: '1:1'}); });
  };
  const harness = new OpenAiHarness(root, invoke, new HarnessPreferences(directory), () => ++factories === 1 ? first : second);
  try {
    await first.login(harness);
    first.onTurn = () => first.emit('item/tool/call', {tool: 'scene_create', callId: 'stable-create', arguments: {name: 'Agent object'}}, 1);
    second.onTurn = () => {
      first.emit('item/tool/call', {tool: 'scene_destroy', arguments: {entity: '0:1'}}, 99);
      second.emit('item/tool/call', {tool: 'scene_create', callId: 'stable-create', arguments: {name: 'Agent object'}}, 55);
    };
    second.onReply = (id, reply) => {
      if (id === 55) {
        assert.deepEqual(JSON.parse(reply.contentItems[0].text), {entity: '1:1'});
        second.emit('item/tool/call', {tool: 'scene_create', callId: 'stable-create', arguments: {name: 'Different object'}}, 56);
      } else {
        assert.equal(reply.success, false, 'reused call ID with changed arguments is refused');
        second.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'completed'}});
      }
    };
    const turn = harness.submit('Create and verify an object', async () => {});
    await until(() => !!finishMutation);
    first.emit('relay/disconnected');
    finishMutation();
    await turn;
    assert.equal(mutations, 1, 'in-flight creation and stale events never repeat mutations');
    assert.equal(factories, 2);
    assert.ok(second.requests.some(r => r.method === 'thread/resume'));
    assert.equal(second.requests.some(r => r.method === 'thread/start'), false);
    const input = second.requests.find(r => r.method === 'turn/start').params.input[0].text;
    assert.match(input, /Completed Relay tool calls/);
    assert.match(input, /Preserved human object/);
    assert.match(input, /1:1/);
    assert.equal(harness.view().status, 'Ready');
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('missing completion notifications are reconciled without interrupting an active reasoning turn', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-completion-'));
  const service = new Service(); let checks = 0, fullReads = 0;
  const original = service.request.bind(service);
  service.request = async (method, params) => {
    if (method === 'thread/read') {assert.equal(params.includeTurns, false); return {thread: {status: {type: ++checks <= 3 ? 'active' : 'idle'}}};}
    if (method !== 'thread/turns/list') return original(method, params);
    assert.equal(params.itemsView, 'summary'); assert.equal(params.limit, 1);
    ++fullReads;
    return {data: [{id: 'turn-fixture', status: 'completed', items: [{type: 'agentMessage', id: 'final', text: 'Verified finished.'}]}]};
  };
  const harness = new OpenAiHarness(root, async () => ({auto_approval: true}), new HarnessPreferences(directory), () => service, 10);
  try {
    await service.login(harness);
    await harness.submit('Long reasoning task', async () => {});
    assert.ok(checks >= 4);
    assert.equal(fullReads, 1);
    assert.equal(service.requests.filter(r => r.method === 'turn/start').length, 1);
    assert.equal(service.requests.some(r => r.method === 'turn/interrupt'), false);
    assert.equal(harness.view().messages.at(-1).content, 'Verified finished.');
    assert.ok(harness.view().openai.diagnostics.some(d => d.event === 'completion_reconciled'));
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('transient stream failures retry remaining work but account failures do not, and raw errors stay private', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-stream-retry-'));
  const service = new Service(); let turns = 0;
  const harness = new OpenAiHarness(root, async () => ({auto_approval: true}), new HarnessPreferences(directory), () => service);
  try {
    await service.login(harness);
    service.onTurn = () => {
      if (++turns === 1) {
        service.emit('error', {error: {message: 'private-provider-secret', codexErrorInfo: {responseStreamDisconnected: {httpStatusCode: 502}}}, willRetry: false});
        service.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'failed'}});
      } else service.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'completed'}});
    };
    await harness.submit('Continue a long task', async () => {});
    assert.equal(turns, 2);
    assert.equal(JSON.stringify(harness.view()).includes('private-provider-secret'), false);
    service.onTurn = () => service.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'failed', error: {message: 'private-auth-error', codexErrorInfo: 'unauthorized'}}});
    await harness.submit('Account unavailable', async () => {});
    assert.equal(service.requests.filter(r => r.method === 'turn/start').length, 3);
    assert.equal(JSON.stringify(harness.view()).includes('private-auth-error'), false);
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('Stop cancels reconnect backoff and project changes refuse subsequent mutations', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-recovery-stop-'));
  const service = new Service(); let factories = 0, mutations = 0, project = 'original';
  const harness = new OpenAiHarness(root, async method => {
    if (method === 'session.status') return {auto_approval: true, project};
    ++mutations; return {};
  }, new HarnessPreferences(directory), () => { ++factories; return service; });
  try {
    await service.login(harness);
    service.onTurn = () => service.emit('relay/disconnected');
    const active = harness.submit('Long task', async () => {});
    await until(() => harness.view().status.startsWith('Reconnecting'));
    harness.cancel(); await active;
    assert.equal(factories, 1);
    assert.equal(harness.view().status, 'Stopped');
    service.onTurn = () => { project = 'changed'; service.emit('item/tool/call', {tool: 'scene_create', arguments: {name: 'Wrong project'}}, 9); };
    await harness.submit('Stay in this project', async () => {});
    assert.equal(mutations, 0);
    assert.equal(service.replies.at(-1).result.success, false);
    assert.match(harness.view().status, /human changed projects/);
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('lost turn acknowledgement observes the accepted turn instead of resubmitting the user request', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-lost-ack-'));
  const service = new Service(); const original = service.request.bind(service);
  service.request = async (method, params) => {
    if (method === 'turn/start') {
      service.requests.push({method, params});
      service.emit('turn/started', {turn: {id: 'turn-fixture'}});
      throw new Error('Simulated RPC timeout');
    }
    if (method === 'thread/read') return {thread: {status: {type: 'idle'}}};
    if (method === 'thread/turns/list') return {data: [{id: 'turn-fixture', status: 'completed', items: []}]};
    return original(method, params);
  };
  const harness = new OpenAiHarness(root, async () => ({auto_approval: true}), new HarnessPreferences(directory), () => service);
  try {
    await service.login(harness);
    await harness.submit('Do not submit twice', async () => {});
    assert.equal(service.requests.filter(r => r.method === 'turn/start').length, 1);
    assert.equal(harness.view().status, 'Ready');
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});


test('repeated outages have bounded retries and failed resume never falls back to replaying in a fresh thread', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-retry-bound-'));
  let factories = 0; const services = [];
  const factory = () => {
    ++factories; const service = new Service(); services.push(service);
    service.account = {type: 'chatgpt', email: 'fixture@example.invalid'};
    service.onTurn = () => service.emit('relay/disconnected'); return service;
  };
  const harness = new OpenAiHarness(root, async () => ({auto_approval: true}), new HarnessPreferences(directory), factory);
  try {
    await harness.submit('Restore connection', async () => {});
    assert.equal(factories, 4, 'initial connection plus three bounded recovery attempts');
    assert.match(harness.view().status, /three attempts/);
    assert.equal(services.flatMap(s => s.requests).filter(r => r.method === 'thread/start').length, 1);
  } finally {harness.close(); rmSync(directory, {recursive: true, force: true});}
  const other = mkdtempSync(path.join(os.tmpdir(), 'relay-resume-refusal-'));
  const first = new Service(), second = new Service(); let count = 0;
  first.account = second.account = {type: 'chatgpt', email: 'fixture@example.invalid'};
  first.onTurn = () => first.emit('relay/disconnected');
  const original = second.request.bind(second);
  second.request = async (method, params) => {if (method === 'thread/resume') throw new Error('Cannot resume original thread'); return original(method, params);};
  const recovery = new OpenAiHarness(root, async () => ({auto_approval: true}), new HarnessPreferences(other), () => ++count === 1 ? first : second);
  try {
    await recovery.submit('Preserve completed work', async () => {});
    assert.match(recovery.view().status, /Cannot resume/);
    assert.equal(second.requests.some(r => r.method === 'thread/start' || r.method === 'turn/start'), false);
  } finally {recovery.close(); rmSync(other, {recursive: true, force: true});}
});

test('OpenAI receives human-selected image inputs and can read attached text without arbitrary filesystem access', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-openai-attachments-'));
  mkdirSync(path.join(directory, 'protocol')); writeFileSync(path.join(directory, 'protocol/relay.protocol.json'), readFileSync(path.join(root, 'protocol/relay.protocol.json')));
  const text = path.join(directory, 'notes.txt'), image = path.join(directory, 'image.png');
  writeFileSync(text, 'Attachment fixture text'); writeFileSync(image, readFileSync(path.join(root, 'tests/fixtures/chat-media/image.png')));
  const service = new Service();
  const harness = new OpenAiHarness(directory, async () => ({auto_approval: true}), new HarnessPreferences(directory), () => service);
  try {
    await service.login(harness);
    service.onTurn = () => service.emit('turn/completed', {turn: {id: 'turn-fixture', status: 'completed'}});
    await harness.submit('', async () => {}, [text, image]);
    const input = service.requests.find(request => request.method === 'turn/start').params.input;
    assert.equal(input[1].type, 'image'); assert.match(input[1].url, /^data:image\/png;base64,/);
    assert.match(input[0].text, /Attachment fixture text/);
    assert.equal(JSON.stringify(harness.view()).includes('data:image'), false);
    assert.ok(service.requests.find(request => request.method === 'thread/start').params.dynamicTools.some(tool => tool.name === 'chat_attachment_read'));
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});

test('usage meters prefer Codex bucket, identify window durations and merge sparse notifications', async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-usage-'));
  const service = new Service(), request = service.request.bind(service);
  let snapshot = {rateLimits: {primary: {usedPercent: 99, windowDurationMins: 300}}, rateLimitsByLimitId: {codex: {limitId: 'codex', primary: {usedPercent: 15, windowDurationMins: 10080}, secondary: {usedPercent: 45, windowDurationMins: 300}}}};
  service.request = async (method, params) => method === 'account/rateLimits/read' ? snapshot : request(method, params);
  const harness = new OpenAiHarness(root, async () => ({}), new HarnessPreferences(directory), () => service);
  try {
    await service.login(harness); await harness.refreshUsage(true);
    assert.deepEqual(harness.view().openai.usage, {weekly: 15, fiveHour: 45});
    service.emit('account/rateLimits/updated', {rateLimits: {limitId: 'codex', secondary: {usedPercent: 81, windowDurationMins: null}, primary: null}});
    await until(() => harness.view().openai.usage.fiveHour === 81);
    assert.equal(harness.view().openai.usage.weekly, 15);
    service.emit('account/rateLimits/updated', {rateLimits: {limitId: 'other', secondary: {usedPercent: 99, windowDurationMins: 300}}});
    await new Promise(resolve => setImmediate(resolve)); assert.equal(harness.view().openai.usage.fiveHour, 81);
    snapshot = {rateLimits: {primary: {usedPercent: 50}, secondary: null}};
    await harness.refreshUsage(true); assert.deepEqual(harness.view().openai.usage, {});
    await harness.control({action: 'signout'}); assert.deepEqual(harness.view().openai.usage, {});
  } finally { harness.close(); rmSync(directory, {recursive: true, force: true}); }
});
