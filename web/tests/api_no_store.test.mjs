import test from 'node:test';
import assert from 'node:assert/strict';

const requests = [];
globalThis.localStorage = {
  getItem(key) {
    return key === 'hivemind_api_key' ? 'test-key' : null;
  },
  setItem() {},
};
globalThis.fetch = async (path, options) => {
  requests.push({ path, options });
  return {
    ok: true,
    status: 200,
    async text() { return '{"entries":[]}'; },
  };
};

const { API } = await import('../dist/js/api.js');

test('dynamic API GET bypasses the browser cache', async () => {
  const result = await API.get('/api/files?worker=droplet&path=');

  assert.deepEqual(result, { entries: [] });
  assert.equal(requests.length, 1);
  assert.equal(requests[0].options.cache, 'no-store');
  assert.equal(requests[0].options.headers.Authorization,
               'Bearer test-key');
});

test('dynamic API mutations also bypass the browser cache', async () => {
  await API.post('/api/files', { path: 'scratch' });

  assert.equal(requests.length, 2);
  assert.equal(requests[1].options.cache, 'no-store');
  assert.equal(requests[1].options.method, 'POST');
});
