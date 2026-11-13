import type { Agent } from '../shared/contracts.js';
import { finalNarrationCredit, takeCompleteSpeech } from '../shared/speech.js';
import { agentAvatar } from './avatars.js';
import { recordingToWav } from './audio.js';
import { byId, errorMessage } from './dom.js';
import type { SpeechPlayer } from './speech-player.js';
import { voiceSphere } from './sphere.js';
import { currentAgent, state } from './state.js';

type VoicePhase = 'idle' | 'listening' | 'thinking' | 'streaming';

interface VoiceCallbacks {
  onSent: () => void;
  onError: (message: string) => void;
}

export class VoiceController {
  private phase: VoicePhase = 'idle';
  private recorder: MediaRecorder | undefined;
  private stream: MediaStream | undefined;
  private chunks: Blob[] = [];
  private discardRecording = false;
  private generation = 0;
  private spokenLength = 0;
  private liveNarrated = '';
  private narratedFinalIds = new Set<string>();
  private sawBusy = false;
  private receivedFinal = false;
  private streamText = '';
  private turnDone = false;
  private stopping = false;
  private queuedBehindStop = false;
  private lastTalkAt = 0;

  constructor(private readonly speech: SpeechPlayer, private readonly callbacks: VoiceCallbacks) {
    byId('talk-button').addEventListener('click', () => { void this.toggleTalk(); });
    byId('voice-stop-button').addEventListener('click', () => { void this.stopCurrent(); });
    byId('voice-back-button').addEventListener('click', () => this.close());
  }

  open(agent: Agent): void {
    this.closeRecorder(true);
    this.speech.stop();
    ++this.generation;
    this.phase = 'idle';
    this.spokenLength = 0;
    this.liveNarrated = '';
    this.narratedFinalIds.clear();
    this.sawBusy = false;
    this.receivedFinal = false;
    this.streamText = '';
    this.turnDone = false;
    this.stopping = false;
    this.queuedBehindStop = false;
    // History already on screen is not new narration.
    for (const message of state.messages) if (message.senderKind === 'agent') this.narratedFinalIds.add(message.id);
    state.voiceOpen = true;
    byId('sidebar').hidden = true;
    byId('voice-panel').hidden = false;
    byId<HTMLImageElement>('voice-avatar').src = agentAvatar(agent);
    byId<HTMLImageElement>('voice-avatar').alt = `${agent.name} avatar`;
    byId('voice-agent-name').textContent = agent.name;
    byId('voice-health').textContent = 'Online';
    byId('voice-health').className = 'pill online';
    this.setStatus('Ready');
    this.syncControls();
  }

  close(): void {
    ++this.generation;
    this.closeRecorder(true);
    this.speech.stop();
    state.voiceOpen = false;
    byId('voice-panel').hidden = true;
    byId('sidebar').hidden = false;
    this.phase = 'idle';
    this.syncControls();
  }

  setHealth(online: boolean, message = ''): void {
    byId('voice-health').textContent = online ? 'Online' : 'Offline';
    byId('voice-health').className = `pill ${online ? 'online' : 'offline'}`;
    if (!online && message) this.setStatus(message);
  }

  update(): void {
    if (!state.voiceOpen) return;
    const agent = currentAgent();
    const agentBusy = state.activity.busy || Boolean(agent?.busy);
    const turn = state.activity.turns.find((item) => !item.done && item.partial);
    const partial = turn?.partial ?? '';
    const error = state.activity.turns.find((item) => item.error)?.error ?? '';
    if (error && this.phase !== 'idle') {
      this.speech.stop();
      this.phase = 'idle';
      this.stopping = false;
      this.setStatus(`Agent error: ${error}`);
      this.notice('Sorry, the agent hit an error and stopped. Please try again.');
      this.syncControls();
      return;
    }
    if (this.stopping) {
      if (agentBusy) {
        this.setStatus('Still busy — the current command must finish first…');
        this.syncControls();
        return;
      }
      this.stopping = false;
      this.queuedBehindStop = false;
      this.phase = 'idle';
      this.turnDone = false;
      this.setStatus('Cancelled');
      this.syncControls();
      return;
    }

    if (agentBusy) this.sawBusy = true;
    if (this.phase === 'thinking' || this.phase === 'streaming') {
      if (this.queuedBehindStop) {
        if (!state.activity.busy) {
          this.setStatus('Waiting for the cancelled turn to finish…');
          this.syncControls();
          return;
        }
        this.queuedBehindStop = false;
      }
      if (partial && !turn?.cancelling) {
        this.phase = 'streaming';
        this.setStatus('Replying…');
        if (!partial.startsWith(this.streamText) || partial.length < this.streamText.length) {
          let prefix = 0;
          while (prefix < partial.length && prefix < this.streamText.length && partial[prefix] === this.streamText[prefix]) ++prefix;
          this.spokenLength = Math.min(this.spokenLength, prefix);
        }
        this.streamText = partial;
        const complete = takeCompleteSpeech(partial.slice(this.spokenLength));
        this.spokenLength = partial.length - complete.rest.length;
        for (const sentence of complete.complete) {
          this.speech.enqueue(sentence, agent?.id ?? '', `voice-${state.currentThreadId}`);
          this.liveNarrated += sentence;
        }
      }
    }

    for (const message of state.messages) {
      if (message.senderKind !== 'agent' || !message.body.trim() || this.narratedFinalIds.has(message.id)) continue;
      this.narratedFinalIds.add(message.id);
      this.receivedFinal = true;
      if (this.phase === 'idle') {
        this.streamText = '';
        this.spokenLength = 0;
        this.liveNarrated = '';
        this.turnDone = true;
      }
      const credit = finalNarrationCredit(this.liveNarrated, message.body.trim());
      const tail = message.body.trim().slice(credit).trim();
      if (tail) {
        this.speech.enqueue(tail, message.senderId || agent?.id || '', `voice-${state.currentThreadId}`);
        this.liveNarrated += tail;
        this.phase = 'streaming';
        this.setStatus('Speaking…');
      }
    }

    if (!agentBusy && (this.sawBusy || this.phase === 'streaming')) this.turnDone = true;
    if (this.turnDone && !agentBusy && this.speech.idle && this.phase !== 'listening') {
      this.phase = 'idle';
      this.turnDone = false;
      this.sawBusy = false;
      this.setStatus(this.receivedFinal ? 'Ready' : 'Turn complete');
    } else if (this.phase === 'idle' && agentBusy) {
      this.setStatus(`${agent?.name ?? 'Agent'} is working…`);
    }
    this.syncControls();
  }

  private async toggleTalk(): Promise<void> {
    const now = Date.now();
    if (now - this.lastTalkAt < 500 || this.stopping) return;
    this.lastTalkAt = now;
    if (this.phase === 'idle') await this.startListening();
    else if (this.phase === 'listening') this.finishListening();
    else if (this.phase === 'streaming') {
      this.speech.stop();
      await this.cancelTurn();
      this.resetNarration();
      this.phase = 'idle';
      await this.startListening();
    }
  }

  private async startListening(): Promise<void> {
    if (!navigator.mediaDevices?.getUserMedia) {
      this.setStatus('Microphone capture is not available in this Electron build');
      return;
    }
    try {
      this.stream = await navigator.mediaDevices.getUserMedia({ audio: true });
      voiceSphere.attachMic(this.stream);
      this.chunks = [];
      this.discardRecording = false;
      const mimeType = ['audio/webm;codecs=opus', 'audio/webm', 'audio/ogg;codecs=opus']
        .find((type) => MediaRecorder.isTypeSupported(type));
      this.recorder = new MediaRecorder(this.stream, mimeType ? { mimeType } : undefined);
      this.recorder.addEventListener('dataavailable', (event) => {
        if (event.data.size) this.chunks.push(event.data);
      });
      this.recorder.addEventListener('stop', () => { void this.finishRecording(); });
      this.recorder.start(250);
      this.phase = 'listening';
      this.setStatus('Listening… tap Send when finished');
      this.syncControls();
    } catch (error) {
      this.closeRecorder(true);
      this.phase = 'idle';
      this.setStatus(`Microphone unavailable: ${errorMessage(error)}`);
      this.syncControls();
    }
  }

  private finishListening(): void {
    if (this.recorder?.state !== 'recording') return;
    this.phase = 'thinking';
    this.setStatus('Transcribing…');
    this.syncControls();
    this.recorder.stop();
    this.stopTracks();
  }

  private async finishRecording(): Promise<void> {
    const generation = this.generation;
    if (this.discardRecording) return;
    try {
      const wav = await recordingToWav(new Blob(this.chunks, { type: this.recorder?.mimeType || 'audio/webm' }));
      if (generation !== this.generation) return;
      this.setStatus('Transcribing…');
      const transcript = await window.hive.transcribe(wav);
      if (generation !== this.generation) return;
      if (!transcript) {
        this.phase = 'idle';
        this.setStatus('Nothing heard — try again');
        this.syncControls();
        return;
      }
      await window.hive.sendMessage(state.currentThreadId, transcript, 'voice');
      if (generation !== this.generation) return;
      const queuedBehindStop = state.activity.turns.some((turn) => turn.cancelling);
      const queued = queuedBehindStop || state.activity.busy || Boolean(currentAgent()?.busy);
      this.resetNarration();
      this.queuedBehindStop = queuedBehindStop;
      this.phase = 'thinking';
      this.setStatus(queued ? `${currentAgent()?.name ?? 'Agent'} is busy — your message is queued…` : 'Waiting for reply…');
      this.syncControls();
      this.callbacks.onSent();
    } catch (error) {
      if (generation !== this.generation) return;
      this.phase = 'idle';
      this.setStatus(errorMessage(error));
      this.callbacks.onError(errorMessage(error));
      this.notice("Your message didn't send. Check the connection and try again.");
      this.syncControls();
    }
  }

  private async stopCurrent(): Promise<void> {
    if (this.phase === 'listening') {
      ++this.generation;
      this.closeRecorder(true);
      this.phase = 'idle';
      this.setStatus('Cancelled');
      this.syncControls();
      return;
    }
    this.speech.stop();
    const wasBusy = state.activity.busy || Boolean(currentAgent()?.busy);
    if (this.phase === 'idle' && currentAgent()?.busy && !state.activity.busy) {
      try { await window.hive.cancelAgent(currentAgent()?.id ?? ''); }
      catch (error) {
        this.setStatus(`Stop failed: ${errorMessage(error)}`);
        this.syncControls();
        return;
      }
    } else {
      if (!await this.cancelTurn()) {
        this.setStatus('Stop failed — try again');
        this.syncControls();
        return;
      }
    }
    if (wasBusy) {
      this.stopping = true;
      this.setStatus('Stopping…');
    } else {
      this.phase = 'idle';
      this.turnDone = false;
      this.setStatus('Cancelled');
    }
    this.syncControls();
  }

  private async cancelTurn(): Promise<boolean> {
    if (!state.currentThreadId) return true;
    try { await window.hive.cancelThread(state.currentThreadId); return true; }
    catch (error) { this.callbacks.onError(errorMessage(error)); return false; }
  }

  private resetNarration(): void {
    this.spokenLength = 0;
    this.liveNarrated = '';
    this.streamText = '';
    this.sawBusy = false;
    this.receivedFinal = false;
    this.turnDone = false;
    this.stopping = false;
    // narratedFinalIds deliberately survives: earlier messages must not replay.
  }

  private notice(text: string): void {
    const agentId = currentAgent()?.id ?? '';
    this.speech.speak(text, agentId, `voice-notice-${state.currentThreadId}`);
  }

  private closeRecorder(discard: boolean): void {
    this.discardRecording = discard;
    if (this.recorder && this.recorder.state !== 'inactive') {
      try { this.recorder.stop(); } catch { /* already stopping */ }
    }
    this.recorder = undefined;
    this.stopTracks();
  }

  private stopTracks(): void {
    for (const track of this.stream?.getTracks() ?? []) track.stop();
    this.stream = undefined;
  }

  private setStatus(message: string): void { byId('voice-status').textContent = message; }

  private syncControls(): void {
    const talk = byId<HTMLButtonElement>('talk-button');
    const stop = byId('voice-stop-button');
    const busy = state.activity.busy || Boolean(currentAgent()?.busy);
    talk.disabled = this.phase === 'thinking' || this.stopping;
    talk.classList.toggle('active', this.phase === 'listening');
    talk.textContent = this.phase === 'listening' ? 'SEND' : this.phase === 'thinking' ? '···' : 'TALK';
    stop.hidden = this.phase === 'idle' && !busy && !this.stopping;
    voiceSphere.setPhase(this.phase === 'streaming' ? 'speaking' : this.phase);
  }
}
