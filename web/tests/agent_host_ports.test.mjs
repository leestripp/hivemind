import test from 'node:test';
import assert from 'node:assert/strict';

const { parseHostTcpPorts } = await import('../dist/js/agent-modal.js');

test('host TCP ports are integers, deduplicated, and numerically sorted', () => {
  assert.deepEqual(parseHostTcpPorts(' 8765, 8090,8765,8188 '),
                   [8090, 8188, 8765]);
  assert.deepEqual(parseHostTcpPorts(''), []);
  assert.deepEqual(parseHostTcpPorts('   '), []);
});

test('host TCP port input rejects every free-form or invalid shape', () => {
  for (const value of ['-T,auto', 'auto', '8090:8090', '1.5', '-1',
                       '0', '65536', '8090;--network=host', '8090 8188',
                       '8090,,8188']) {
    assert.throws(() => parseHostTcpPorts(value));
  }
  assert.throws(() => parseHostTcpPorts(
    Array.from({ length: 65 }, (_, index) => index + 1).join(',')));
  assert.equal(parseHostTcpPorts(
    Array.from({ length: 64 }, (_, index) => index + 1).join(',')).length, 64);
});
