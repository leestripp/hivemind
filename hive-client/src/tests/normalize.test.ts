import assert from 'node:assert/strict';
import test from 'node:test';
import { normalizeActivity, normalizeBootstrap, normalizeMessagePage, normalizeRoutine } from '../shared/normalize.js';

test('defensive hub normalization ignores malformed records', () => {
  const result = normalizeBootstrap(
    { agents: [{ id: 'a1', name: 'Bob', state: 'READY', busy: true }, { name: 'missing id' }, null] },
    { workers: [{ name: 'local', online: true }] },
    { threads: [{ id: 't1', agent_ids: ['a1'], unread: 2 }, null] },
  );
  assert.equal(result.agents.length, 1);
  assert.equal(result.agents[0]?.state, 'ready');
  assert.equal(result.threads[0]?.id, 't1');
});

test('message and activity payloads retain useful partial data', () => {
  const page = normalizeMessagePage({ messages: [
    { id: 'm1', sender_kind: 'agent', body: 'Hello', attachments: [{ name: 'x.txt', size: 3 }] },
  ], has_more: true });
  assert.equal(page.messages[0]?.attachments[0]?.name, 'x.txt');
  assert.equal(page.hasMore, true);
  const activity = normalizeActivity({ busy: true, turns: [{ agent_id: 'a1', partial: 'Working', done: false }] });
  assert.equal(activity.turns[0]?.partial, 'Working');
  assert.equal(activity.turns[0]?.done, false);
});

test('routine normalization preserves schedule variants and worker attachment routing', () => {
  const routine = normalizeRoutine({ id: 'r1', agent_id: 'a1', name: 'Weekly', prompt: 'Work',
    schedule: { type: 'weekly', time: '09:30', days: [1, 5] }, enabled: false });
  assert.equal(routine.schedule.type, 'weekly');
  assert.deepEqual(routine.schedule.type === 'weekly' ? routine.schedule.days : [], [1, 5]);
  assert.equal(routine.enabled, false);
  const page = normalizeMessagePage({ messages: [{ id: 'm1', attachments: [{ name: 'x.png', path: '/workspace/x.png', worker: 'rig' }] }] });
  assert.equal(page.messages[0]?.attachments[0]?.worker, 'rig');
});
