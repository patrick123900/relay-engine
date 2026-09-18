import assert from 'node:assert/strict';
import { test } from 'node:test';
import { mkdtempSync, readFileSync, statSync, rmSync, symlinkSync, mkdirSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ProviderStore } from '../dist/provider.js';
import { ChatWorkflow } from '../dist/chat.js';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');

test('private authentication persists with restrictive permissions, is redacted and ignores symlinks', () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-provider-'));
  try {
    const store = new ProviderStore(directory, {});
    store.save({ endpoint: 'http://localhost:1234/v1/chat/completions', model: 'fixture', auth: 'header', header: 'x-api-key', credential: 'fixture-private-value' });
    const filename = path.join(directory, '.relay/agent-provider.json');
    assert.equal(statSync(filename).mode & 0o777, 0o600);
    assert.equal(JSON.stringify(store.view()).includes('fixture-private-value'), false);
    assert.equal(new ProviderStore(directory, {}).current().apiKey, 'fixture-private-value');
    store.save({ ...store.view(), credential: '' });
    assert.equal(store.current().apiKey, 'fixture-private-value');
    store.save({ ...store.view(), credential: '', clear: true });
    assert.equal(store.current().apiKey, '');
    assert.equal(readFileSync(filename, 'utf8').includes('fixture-private-value'), false);
    assert.throws(() => store.save({ ...store.view(), credential: 'bad\nheader' }));
    assert.throws(() => store.save({ ...store.view(), header: 'Host', credential: 'value' }));
    rmSync(filename);
    const outside = path.join(directory, 'outside.json'); writeFileSync(outside, 'unchanged');
    symlinkSync(outside, filename);
    assert.throws(() => store.save({ ...store.view(), credential: 'value' }));
    assert.equal(readFileSync(outside, 'utf8'), 'unchanged');
    rmSync(filename); writeFileSync(filename, 'broken fixture-private-value');
    assert.throws(() => new ProviderStore(directory, {}), error => !error.message.includes('fixture-private-value'));
    rmSync(path.join(directory, '.relay'), { recursive: true }); mkdirSync(path.join(directory, 'outside'));
    symlinkSync(path.join(directory, 'outside'), path.join(directory, '.relay'));
    assert.throws(() => new ProviderStore(directory, {}));
    const ignored = spawnSync('git', ['check-ignore', '.relay/agent-provider.json', '.relay/.provider-fixture.tmp', '.relay/agent-provider.json.bak', '.relay/openai/auth.json', '.relay/openai/log/codex.log', '.relay/agent-harness.json'], { cwd: root, encoding: 'utf8' });
    assert.equal(ignored.status, 0);
    assert.equal(ignored.stdout.trim().split('\n').length, 6);
  } finally { rmSync(directory, { recursive: true, force: true }); }
});

test('full Auto approval continues beyond scoped turn limits and includes replay', async () => {
  const original = globalThis.fetch;
  let rounds = 0, calls = 0;
  const workflow = new ChatWorkflow(root, async (method) => {
    if (method === 'session.status') return { auto_approval: true };
    assert.equal(method, 'runtime.status'); ++calls; return { frame: calls };
  }, { endpoint: 'http://localhost:1', model: 'fixture', apiKey: 'fixture-value', authHeader: 'x-api-key', authPrefix: '' });
  globalThis.fetch = async (_, options) => {
    assert.equal(options.headers['x-api-key'], 'fixture-value');
    const body = JSON.parse(options.body);
    assert.ok(body.tools.some(tool => tool.function.name === 'trace_replay'));
    ++rounds;
    return new Response(JSON.stringify({ choices: [{ message: { role: 'assistant', content: rounds > 10 ? 'Done' : null, ...(rounds <= 10 ? { tool_calls: Array.from({length: 2}, (_, index) => ({ id: `call-${rounds}-${index}`, type: 'function', function: { name: 'runtime_status', arguments: '{}' } })) } : {}) } }] }));
  };
  try { await workflow.submit('Continue until complete', async () => {}); assert.equal(calls, 20); assert.equal(workflow.view().status, 'Ready'); }
  finally { globalThis.fetch = original; workflow.close(); }
});

test('public editor command delegates to bridge before initializing any desktop surface', () => {
  if (process.platform === 'win32') return;
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-editor-launch-'));
  try {
    const executable = path.join(directory, 'fake-node');
    writeFileSync(executable, '#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n', { mode: 0o700 });
    const binary = process.env.RELAY_ENGINE_BINARY ?? path.join(root, 'build/dev/relay_demo');
    const result = spawnSync(binary, ['--editor'], { cwd: directory, env: { ...process.env, RELAY_NODE_EXECUTABLE: executable, DISPLAY: ':65535', WAYLAND_DISPLAY: '' }, encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    assert.deepEqual(JSON.parse(result.stdout), [path.join(root, 'tools/mcp-bridge/dist/index.js'), '--editor', '--engine-binary', binary]);
  } finally { rmSync(directory, { recursive: true, force: true }); }
});
