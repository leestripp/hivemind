/** Split only at safe spoken boundaries; the final partial sentence stays put. */
export function takeCompleteSpeech(text: string): { complete: string[]; rest: string } {
  const complete: string[] = [];
  let start = 0;
  const boundary = /[.!?…]+(?:["'”’)\]]*)\s+|[\n\r]+|(?<=[。！？])/gu;
  for (const match of text.matchAll(boundary)) {
    const end = (match.index ?? 0) + match[0].length;
    const sentence = text.slice(start, end).trim();
    if (sentence.length > 0) complete.push(sentence);
    start = end;
  }
  return { complete, rest: text.slice(start) };
}

/** Strip presentation markup without ever byte-splitting Unicode text. */
export function cleanForSpeech(text: string): string {
  return text
    .replace(/```[\s\S]*?```/g, ' code example omitted. ')
    .replace(/`([^`]+)`/g, '$1')
    .replace(/!\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
    .replace(/<[^>]+>/g, ' ')
    .replace(/^\s{0,3}#{1,6}\s+/gm, '')
    .replace(/^\s*[-*+]\s+/gm, '')
    .replace(/[*_~|]/g, '')
    .replace(/[ \t]+/g, ' ')
    .trim();
}

/** Find the final message prefix already present at the end of live narration. */
export function finalNarrationCredit(liveNarrated: string, finalText: string): number {
  const maximum = Math.min(liveNarrated.length, finalText.length);
  for (let length = maximum; length > 0; --length) {
    if (liveNarrated.endsWith(finalText.slice(0, length))) return length;
  }
  return 0;
}

export function speechChunks(text: string, maximumLength = 900): string[] {
  const clean = cleanForSpeech(text);
  if (clean.length <= maximumLength) return clean ? [clean] : [];
  const output: string[] = [];
  let remaining = clean;
  while (remaining.length > maximumLength) {
    const window = remaining.slice(0, maximumLength + 1);
    const punctuation = Math.max(window.lastIndexOf('. '), window.lastIndexOf('! '), window.lastIndexOf('? '), window.lastIndexOf('\n'));
    const split = punctuation > maximumLength / 2 ? punctuation + 1 : window.lastIndexOf(' ');
    const end = split > maximumLength / 2 ? split : maximumLength;
    output.push(remaining.slice(0, end).trim());
    remaining = remaining.slice(end).trim();
  }
  if (remaining) output.push(remaining);
  return output.filter(Boolean);
}
