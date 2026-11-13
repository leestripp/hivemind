import { speechChunks } from '../shared/speech.js';
import { voiceSphere } from './sphere.js';
import { state } from './state.js';

interface SpeechRequest {
  text: string;
  agentId: string;
  key: string;
  model?: string;
  voice?: string;
}

export class SpeechPlayer {
  private queue: SpeechRequest[] = [];
  private epoch = 0;
  private running = false;
  private audio: HTMLAudioElement | undefined;
  private audioUrl = '';
  private finishCurrent: (() => void) | undefined;
  private active = '';

  constructor(private readonly onError: (message: string) => void, private readonly onIdle: () => void) {}

  get activeKey(): string { return this.active; }
  get idle(): boolean { return !this.running && this.queue.length === 0; }

  speak(text: string, agentId: string, key: string, model?: string, voice?: string): void {
    const chunks = speechChunks(text);
    if (!chunks.length) return;
    this.stop();
    const epoch = this.epoch;
    this.queue = chunks.map((chunk): SpeechRequest => ({
      text: chunk, agentId, key, ...(model ? { model } : {}), ...(voice !== undefined ? { voice } : {}),
    }));
    this.active = key;
    void this.pump(epoch);
  }

  enqueue(text: string, agentId: string, key: string): void {
    const chunks = speechChunks(text);
    if (!chunks.length) return;
    this.queue.push(...chunks.map((chunk) => ({ text: chunk, agentId, key })));
    this.active ||= key;
    if (!this.running) void this.pump(this.epoch);
  }

  setVolume(): void {
    if (this.audio) this.audio.volume = state.muted ? 0 : state.settings.masterVolume;
  }

  stop(): void {
    ++this.epoch;
    this.queue = [];
    this.active = '';
    this.audio?.pause();
    this.finishCurrent?.();
    this.cleanupAudio();
    this.running = false;
    this.onIdle();
  }

  private async pump(epoch: number): Promise<void> {
    if (this.running) return;
    this.running = true;
    try {
      while (epoch === this.epoch && this.queue.length) {
        const request = this.queue.shift()!;
        try {
          const audio = await window.hive.synthesize(request.text, request.agentId, request.model ?? '', request.voice);
          if (epoch !== this.epoch) return;
          await this.play(audio.bytes, audio.mimeType);
        } catch (error) {
          if (epoch === this.epoch) this.onError(error instanceof Error ? error.message : String(error));
        }
      }
    } finally {
      if (epoch === this.epoch) {
        this.running = false;
        this.active = '';
        this.cleanupAudio();
        this.onIdle();
      }
    }
  }

  private play(bytes: ArrayBuffer, mimeType: string): Promise<void> {
    this.cleanupAudio();
    this.audioUrl = URL.createObjectURL(new Blob([bytes], { type: mimeType }));
    const audio = new Audio(this.audioUrl);
    voiceSphere.attachPlayback(audio);
    this.audio = audio;
    audio.volume = state.muted ? 0 : state.settings.masterVolume;
    return new Promise<void>((resolve) => {
      let finished = false;
      const finish = () => {
        if (finished) return;
        finished = true;
        this.finishCurrent = undefined;
        resolve();
      };
      this.finishCurrent = finish;
      audio.addEventListener('ended', finish, { once: true });
      audio.addEventListener('error', finish, { once: true });
      void audio.play().catch(finish);
    });
  }

  private cleanupAudio(): void {
    if (this.audio) {
      this.audio.pause();
      this.audio.removeAttribute('src');
      this.audio.load();
      this.audio = undefined;
    }
    if (this.audioUrl) URL.revokeObjectURL(this.audioUrl);
    this.audioUrl = '';
  }
}
