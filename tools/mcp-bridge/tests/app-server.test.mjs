// Real App Server against a loopback Responses fixture: no OpenAI account or model calls.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {createServer} from 'node:http';
import {spawnSync} from 'node:child_process';
import {mkdtempSync, rmSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {CodexTransport} from '../dist/openai.js';

const executable = process.env.RELAY_CODEX_EXECUTABLE ?? 'codex';
test('real App Server persists tools/history and resumes after process restart against a local Responses fixture', {timeout: 20000}, async context => {
  if (spawnSync(executable, ['--version'], {encoding: 'utf8'}).status !== 0) {context.skip('Codex unavailable'); return;}
  let requests = 0;
  const bodies = [];
  const server = createServer(async (req, res) => {
    let text = ''; for await (const bytes of req) text += bytes;
    bodies.push(JSON.parse(text)); ++requests;
    assert.equal(req.headers.authorization, undefined, 'local fixture needs no account credentials');
    res.writeHead(200, {'content-type': 'text/event-stream'});
    const id = `resp-${requests}`;
    const emit = (type, extra) => res.write(`data: ${JSON.stringify({type, ...extra})}\n\n`);
    const response = {id, object: 'response', status: 'in_progress', model: 'fixture-model', output: []};
    emit('response.created', {response});
    const item = requests === 1
      ? {type: 'function_call', id: 'fc-1', call_id: 'persisted-call', name: 'scene_create', arguments: JSON.stringify({name: 'Fixture object'})}
      : {type: 'message', id: `msg-${requests}`, role: 'assistant', status: 'completed', content: [{type: 'output_text', text: 'Local fixture completed.', annotations: []}]};
    emit('response.output_item.added', {output_index: 0, item});
    emit('response.output_item.done', {output_index: 0, item});
    emit('response.completed', {response: {...response, status: 'completed', output: [item], usage: {input_tokens: 10, output_tokens: 5, total_tokens: 15}}});
    res.end();
  });
  await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);});
  const directory = mkdtempSync(path.join(os.tmpdir(), 'relay-real-resume-'));
  const overrides = ['model_provider="relay_fixture"', 'model_providers.relay_fixture.name="Local fixture"',
    `model_providers.relay_fixture.base_url="http://127.0.0.1:${server.address().port}"`,
    'model_providers.relay_fixture.wire_api="responses"', 'model_providers.relay_fixture.requires_openai_auth=false'];
  let service;
  const connect = async () => {
    service = new CodexTransport(directory, executable, overrides);
    await service.request('initialize', {clientInfo: {name: 'relay_local_fixture', version: '0.1'}, capabilities: {experimentalApi: true}});
    service.notify('initialized');
  };
  const turn = async threadId => {
    let resolve, reject;
    const done = new Promise((yes, no) => {resolve = yes; reject = no;});
    service.onMessage = (method, params, id) => {
      if (method === 'item/tool/call') {
        assert.equal(params.tool, 'scene_create'); assert.equal(params.callId, 'persisted-call');
        service.reply(id, {success: true, contentItems: [{type: 'inputText', text: JSON.stringify({entity: '0:1'})}]});
      }
      if (method === 'turn/completed') params.turn.status === 'completed' ? resolve() : reject(new Error(`fixture turn ${params.turn.status}`));
    };
    await service.request('turn/start', {threadId, model: 'fixture-model', input: [{type: 'text', text: 'Use the fixture tool and finish.'}]});
    await done;
  };
  try {
    await connect();
    const started = await service.request('thread/start', {cwd: path.join(directory, '.relay/openai/workspace'), model: 'fixture-model', modelProvider: 'relay_fixture', ephemeral: false, approvalPolicy: 'never', sandbox: 'read-only',
      dynamicTools: [{type: 'function', name: 'scene_create', description: 'Create a fixture entity', inputSchema: {type: 'object', properties: {name: {type: 'string'}}, required: ['name']}}], baseInstructions: 'Use only the provided fixture tools.'});
    await turn(started.thread.id);
    service.close();
    await connect();
    const resumed = await service.request('thread/resume', {threadId: started.thread.id, model: 'fixture-model', modelProvider: 'relay_fixture', approvalPolicy: 'never', sandbox: 'read-only', excludeTurns: true});
    assert.equal(resumed.thread.id, started.thread.id);
    assert.equal(resumed.thread.turns.length, 0, 'resume avoids hydrating potentially large capture history');
    const history = await service.request('thread/turns/list', {threadId: started.thread.id, limit: 1, sortDirection: 'desc', itemsView: 'summary'});
    assert.equal(history.data.length, 1, 'completed turn survives process restart');
    assert.equal(history.data[0].status, 'completed');
    await turn(started.thread.id);
    assert.ok(JSON.stringify(bodies.at(-1)).includes('\"name\":\"scene_create\"'), 'dynamic tools are restored from rollout metadata');
    assert.ok(JSON.stringify(bodies.at(-1).input).includes('0:1'), 'completed tool result survives restart');
  } finally {
    service?.close(); server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    rmSync(directory, {recursive: true, force: true});
  }
});
