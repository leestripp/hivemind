import assert from 'node:assert/strict';
import test from 'node:test';
import { cleanForSpeech, finalNarrationCredit, speechChunks, takeCompleteSpeech } from '../shared/speech.js';

test('live speech splits only complete boundaries', () => {
  assert.deepEqual(takeCompleteSpeech('First sentence. Second sentence! A partial'), {
    complete: ['First sentence.', 'Second sentence!'], rest: 'A partial',
  });
  assert.deepEqual(takeCompleteSpeech('你好。下一句！还没完'), { complete: ['你好。', '下一句！'], rest: '还没完' });
});

test('final narration credit suppresses the already spoken prefix only', () => {
  assert.equal(finalNarrationCredit('Hello world. Next', 'Hello world. Next sentence.'), 17);
  assert.equal(finalNarrationCredit('A different reply.', 'Hello world.'), 0);
});

test('speech cleanup and chunking preserve Unicode', () => {
  assert.equal(cleanForSpeech('## Heading\n**Hello** [world](https://example.invalid) `code`'), 'Heading\nHello world code');
  const chunks = speechChunks('🐝'.repeat(1000), 50);
  assert.ok(chunks.length > 1);
  assert.equal(chunks.join(''), '🐝'.repeat(1000));
  for (const chunk of chunks) assert.equal(chunk.includes('\uFFFD'), false);
});
