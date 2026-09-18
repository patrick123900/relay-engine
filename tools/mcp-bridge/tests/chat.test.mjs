import assert from 'node:assert/strict';
import { test } from 'node:test';
import { spawn } from 'node:child_process';
import readline from 'node:readline';
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ChatWorkflow, chatConfiguration, engineEnvironment } from '../dist/chat.js';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const token = 'b'.repeat(64);
const config = chatConfiguration({ RELAY_CHAT_ENDPOINT: 'http://127.0.0.1:1/v1/chat/completions', RELAY_CHAT_MODEL: 'fixture-model', RELAY_CHAT_API_KEY: 'fixture-provider-secret' });
const tool = (name, args, id = 'call-1') => ({ id, type: 'function', function: { name, arguments: JSON.stringify(args) } });
const answer = (content, calls) => new Response(JSON.stringify({ choices: [{ message: { role: 'assistant', content, ...(calls ? { tool_calls: calls } : {}) } }] }));

test('configuration and engine environment exclude provider credentials', () => {
  const env = engineEnvironment({ RELAY_CHAT_API_KEY: 'private', OPENAI_API_KEY: 'private', CUSTOM_PROVIDER: 'private', CUSTOM_SECRET: 'private', RELAY_CHAT_MODEL: 'private', RELAY_AGENT_GRANTS: 'scene.list', DISPLAY: ':65535', PATH: '/usr/bin' }, token);
  assert.equal(env.RELAY_AGENT_GRANTS, 'scene.list');
  assert.equal(env.DISPLAY, ':65535');
  assert.equal(env.RELAY_BRIDGE_TOKEN, token);
  assert.equal(env.OPENAI_API_KEY, undefined);
  assert.equal(env.RELAY_CHAT_API_KEY, undefined);
  assert.equal(env.RELAY_CHAT_MODEL, undefined);
  assert.equal(env.CUSTOM_PROVIDER, undefined);
  assert.equal(env.CUSTOM_SECRET, undefined);
  assert.throws(() => chatConfiguration({ RELAY_CHAT_MODEL: 'fixture', RELAY_CHAT_ENDPOINT: 'http://example.com/chat' }));
  assert.throws(() => chatConfiguration({ RELAY_CHAT_MODEL: 'fixture', RELAY_CHAT_ENDPOINT: 'https://user:secret@example.com/chat' }));
  assert.equal(chatConfiguration({}), undefined);
});

test('external workflow integrates native approval, target scopes and bridge display', async () => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'relay-chat-test-'));
  const child = spawn(process.env.RELAY_SESSION_TEST_HOST ?? path.join(root, 'build/dev/relay_session_test_host'), [], {
    cwd: directory, env: engineEnvironment({ ...process.env, RELAY_AGENT_GRANTS: '' }, token), stdio: ['pipe', 'pipe', 'pipe'],
  });
  const pending = new Map();
  let serial = 0;
  let stderr = '';
  child.stderr.on('data', chunk => { stderr += chunk; });
  readline.createInterface({ input: child.stdout }).on('line', line => {
    const result = JSON.parse(line);
    const resolver = pending.get(result.id);
    assert.ok(resolver, line);
    pending.delete(result.id);
    result.ok ? resolver.resolve(result.result) : resolver.reject(new Error(result.error));
  });
  child.on('exit', () => { for (const resolver of pending.values()) resolver.reject(new Error(`test host exited: ${stderr}`)); });
  const call = (method, parameters = {}, host = false) => new Promise((resolve, reject) => {
    const id = ++serial;
    pending.set(id, { resolve, reject });
    child.stdin.write(JSON.stringify({ host, request: { ...parameters, method, id } }) + '\n');
  });
  const bridge = (method, parameters = {}) => call(method, { ...parameters, bridge_token: token });
  const originalFetch = globalThis.fetch;
  let workflow;
  try {
    const created = await call('scene.create', { name: 'Limited target' }, true);
    const other = await call('scene.create', { name: 'Other target' }, true);
    await assert.rejects(call('bridge.poll'));
    await assert.rejects(call('bridge.poll', { bridge_token: 'wrong' }));
    await assert.rejects(call('session.grant', { scope: 'scene.clear' }));
    await bridge('bridge.publish', { view: JSON.stringify({ status: 'Connected', messages: [] }) });
    await call('chat.submit', { message: 'Move only the approved target' }, true);
    const submissions = await bridge('bridge.poll');
    assert.equal(submissions.submissions[0].message, 'Move only the approved target');
    assert.deepEqual((await bridge('bridge.poll')).submissions, []);
    let calls = 0;
    globalThis.fetch = async (_url, options) => {
      ++calls;
      const body = JSON.parse(options.body);
      assert.equal(options.redirect, 'error');
      assert.equal(options.headers.Authorization, 'Bearer fixture-provider-secret');
      assert.equal(body.store, false);
      assert.equal(body.model, 'fixture-model');
      const names = body.tools.map(tool => tool.function.name);
      assert.ok(!names.includes('session_grant') && !names.includes('chat_submit') && !names.includes('bridge_publish'));
      assert.ok(names.includes('session_request'));
      if (calls === 1) return answer(null, [tool('session_request', { scope: 'scene.set_transform', kind: 'entity', target: created.entity })]);
      if (calls === 2) return answer(null, [tool('scene_set_transform', { entity: created.entity, px: 8, gesture: 37 }), tool('scene_set_transform', { entity: other.entity, px: 99 }, 'call-2')]);
      assert.ok(body.messages.some(message => message.role === 'tool' && message.content.includes('capability denied')));
      return answer('The approved target moved; access to the other target was denied.');
    };
    workflow = new ChatWorkflow(root, call, config);
    const publish = () => bridge('bridge.publish', { view: JSON.stringify(workflow.view()) });
    await workflow.submit(submissions.submissions[0].message, publish);
    assert.equal(calls, 1, 'approval pauses provider loop');
    assert.match(workflow.view().status, /Waiting for Access review/);
    const review = await call('session.review', {}, true);
    assert.equal(review.pending.length, 1);
    await call('session.decide', { request: review.pending[0].request, allow: true }, true);
    await workflow.submit('Access approved; continue', publish);
    assert.equal(calls, 3);
    const approved = await call('scene.inspect', { entity: created.entity }, true);
    const denied = await call('scene.inspect', { entity: other.entity }, true);
    assert.equal(approved.transform.position.x, 8);
    assert.equal(denied.transform.position.x, 0);
    const audit = await call('session.audit');
    assert.ok(audit.entries.some(entry => entry.scope === 'scene.set_transform' && !entry.allowed));
    assert.ok(audit.entries.some(entry => entry.scope === 'scene.set_transform' && entry.allowed && entry.scoped_grants.some(grant => grant.target === created.entity)));
    const view = await call('chat.status', {}, true);
    assert.ok(view.connected && view.view.messages.some(message => message.role === 'assistant'));
    assert.ok(view.view.results.some(result => !result.succeeded));
    assert.ok(!JSON.stringify(view).includes('fixture-provider-secret'));
    await call('session.revoke', {}, true);
    await assert.rejects(call('scene.set_transform', { entity: created.entity, px: 10 }));
    await call('chat.cancel', {}, true);
    assert.equal((await bridge('bridge.poll')).submissions[0].cancel, true);
  } finally {
    workflow?.close();
    globalThis.fetch = originalFetch;
    child.stdin.end();
    if (child.exitCode === null) await new Promise(resolve => child.once('exit', resolve));
    await rm(directory, { recursive: true, force: true });
  }
});

test('provider errors are bounded and redacted, cancellation aborts requests', async () => {
  const originalFetch = globalThis.fetch;
  const invoke = async method => { assert.equal(method, 'session.status'); return { grants: [] }; };
  const workflow = new ChatWorkflow(root, invoke, config);
  try {
    globalThis.fetch = async () => { throw new Error('provider echoed fixture-provider-secret'); };
    await workflow.submit('test', async () => {});
    assert.ok(!JSON.stringify(workflow.view()).includes('fixture-provider-secret'));
    let started;
    const starting = new Promise(resolve => { started = resolve; });
    globalThis.fetch = async (_url, options) => {
      started();
      return new Promise((_resolve, reject) => options.signal.addEventListener('abort', () => reject(new Error('Chat stopped')), { once: true }));
    };
    const turn = workflow.submit('cancel me', async () => {});
    await starting;
    workflow.cancel();
    await turn;
    assert.equal(workflow.view().busy, false);
    assert.match(workflow.view().status, /stopped/);
    let rounds = 0;
    globalThis.fetch = async () => ++rounds === 1
      ? answer(null, [tool('scene_create', { name: 'fixture-provider-secret' })])
      : answer('Credential-bearing tool was refused.');
    await workflow.submit('refuse credential forwarding', async () => {});
    assert.ok(workflow.view().results.some(result => result.summary.includes('Credential-bearing')));
    globalThis.fetch = async () => new Response('x', { headers: { 'content-length': '262145' } });
    await workflow.submit('too big', async () => {});
    assert.match(workflow.view().status, /limit/);
    globalThis.fetch = async () => answer('a'.repeat(1499) + '😀');
    await workflow.submit('unicode boundary', async () => {});
    assert.ok(workflow.view().messages.every(message => message.content.isWellFormed()));
  } finally { workflow.close(); globalThis.fetch = originalFetch; }
});
