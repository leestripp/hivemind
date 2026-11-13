import type { ChimpFlowCatalog, SpeechAudio } from '../shared/contracts.js';
import { objectValue, stringValue } from '../shared/normalize.js';
import { DEFAULT_CHIMPFLOW_MODEL, type SettingsStore } from './config.js';
import { discoveryIds, requestBytes, requestJson, responseError } from './http.js';
import { normalizeWavLoudness, wavLufs } from './loudness.js';

function assertWav(bytes: ArrayBuffer): void {
  const view = new DataView(bytes);
  if (bytes.byteLength < 44 || view.getUint32(0, false) !== 0x52494646 || view.getUint32(8, false) !== 0x57415645) {
    throw new Error('ChimpFlow returned malformed WAV audio');
  }
}

export class ChimpFlowService {
  constructor(private readonly settings: SettingsStore, private readonly log: (message: string) => void = () => {}) {}

  private connection(): { url: string; key: string; model: string } {
    const settings = this.settings.snapshot();
    return { url: settings.chimpflowUrl, key: this.settings.key(), model: settings.chimpflowModel };
  }

  async healthy(): Promise<boolean> {
    const { url } = this.connection();
    try {
      const response = await requestBytes(url, '/healthz', '', {}, 5_000);
      return response.status === 200 && new TextDecoder().decode(response.bytes).includes('"ok"');
    } catch { return false; }
  }

  async catalog(requestedModel?: string): Promise<ChimpFlowCatalog> {
    const { url, key, model: savedModel } = this.connection();
    let model = requestedModel || savedModel || DEFAULT_CHIMPFLOW_MODEL;
    if (!key) return { online: false, models: [], model, voices: [], error: 'Enter the ChimpFlow API key, then click Save.' };
    try {
      if (!await this.healthy()) {
        return { online: false, models: [], model, voices: [], error: 'ChimpFlow is offline. Check the address, port, and API key.' };
      }
      const models = discoveryIds(await requestJson(url, '/v1/models', key), 'model');
      if (!models.length) throw new Error('ChimpFlow returned no speech models');
      if (!models.includes(model)) model = models.includes(DEFAULT_CHIMPFLOW_MODEL) ? DEFAULT_CHIMPFLOW_MODEL : models[0]!;
      const voices = await this.voices(model);
      return { online: true, models, model, voices, error: voices.length ? '' : `No voices available for ${model}` };
    } catch (error) {
      return { online: false, models: [], model, voices: [], error: error instanceof Error ? error.message : String(error) };
    }
  }

  async voices(model: string): Promise<string[]> {
    if (!model) throw new Error('A ChimpFlow model is required for voice discovery');
    const { url, key } = this.connection();
    return discoveryIds(await requestJson(url, `/v1/voices?model=${encodeURIComponent(model)}`, key), 'voice');
  }

  async synthesize(text: string, agentId: string, requestedModel?: string, requestedVoice?: string): Promise<SpeechAudio> {
    const { url, key, model: savedModel } = this.connection();
    const model = requestedModel || savedModel || DEFAULT_CHIMPFLOW_MODEL;
    const voice = requestedVoice ?? this.settings.snapshot().voices[model]?.[agentId] ?? '';
    const response = await requestBytes(url, '/v1/audio/speech', key, {
      method: 'POST', headers: { 'Content-Type': 'application/json', Accept: 'audio/wav' },
      body: JSON.stringify({ model, input: text, response_format: 'wav', ...(voice ? { voice } : {}) }),
    }, 60_000);
    if (response.status !== 200) throw responseError(response.status, response.bytes, 'Speech request failed');
    if (!response.contentType.toLowerCase().includes('audio/wav') &&
      !response.contentType.toLowerCase().includes('audio/x-wav')) {
      throw new Error(`ChimpFlow returned ${response.contentType || 'an unknown type'} instead of WAV`);
    }
    assertWav(response.bytes);
    const fallback = response.headers.get('x-chimpflow-voice-fallback');
    if (fallback) console.warn(`ChimpFlow replaced unavailable voice ${fallback} with the model default`);
    // Same loudness whatever voice the agent wears — the catalog spans
    // ~6 dB RMS voice-to-voice otherwise (GTK client parity).
    const before = wavLufs(response.bytes);
    const normalized = normalizeWavLoudness(response.bytes);
    const after = wavLufs(normalized);
    const db = (value: number | undefined): string => value === undefined ? 'n/a' : value.toFixed(1);
    this.log(`synthesize model=${model} voice=${voice || '(default)'} bytes=${normalized.byteLength} lufs ${db(before)} -> ${db(after)} LUFS`);
    return { bytes: normalized, mimeType: 'audio/wav' };
  }

  async transcribe(wav: ArrayBuffer): Promise<string> {
    assertWav(wav);
    if (wav.byteLength > 25 * 1024 * 1024) throw new Error('Recording is too large to transcribe');
    const { url, key } = this.connection();
    const form = new FormData();
    form.append('model', 'whisper');
    form.append('file', new Blob([wav], { type: 'audio/wav' }), 'utterance.wav');
    const response = await requestBytes(url, '/v1/audio/transcriptions', key,
      { method: 'POST', body: form }, 60_000);
    if (response.status !== 200) throw responseError(response.status, response.bytes, 'Transcription failed');
    try {
      const root = objectValue(JSON.parse(new TextDecoder().decode(response.bytes)));
      return stringValue(root.text).trim();
    } catch { throw new Error('ChimpFlow returned an unreadable transcription'); }
  }
}
