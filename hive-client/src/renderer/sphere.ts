// ==== Voice sphere — 2D canvas interpretation of the GTK client's GL
// "swarm + honeycomb" (voice/src/SphereArea.cpp). Where the GTK version
// projects 3D insects onto a shader hex backdrop, this one goes for the
// strongest 2D patterns: a slow-drifting honeycomb across the whole voice
// panel, a smooth morphing frequency BLOB (not a plain ring) with outer
// tick bars and counter-rotating dashed arcs, an inner mirrored reflection,
// and a constellation particle swarm. ====

import { byId } from './dom.js';

export type SpherePhase = 'idle' | 'listening' | 'thinking' | 'speaking';

const TAU = Math.PI * 2;
const BINS = 64;          // frequency points around the blob
const RAYS = 140;         // individual radial waveform rays
const PARTICLES = 36;

// ---- value-noise fBm: organic, never-repeating motion for the idle orb ----
function hash2(x: number, y: number): number {
  const h = Math.sin(x * 127.1 + y * 311.7) * 43758.5453123;
  return h - Math.floor(h);
}
function vnoise(x: number, y: number): number {
  const xi = Math.floor(x), yi = Math.floor(y);
  const xf = x - xi, yf = y - yi;
  const u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
  const a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
  return (a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v) * 2 - 1;
}
function fbm(x: number, y: number): number {
  return vnoise(x, y) * 0.65 + vnoise(x * 2.1 + 5.2, y * 2.1 + 1.3) * 0.35;
}

// GTK shader palette: idle teal / listen cyan / think amber / speak green.
const PHASE_STYLE: Record<SpherePhase, { rgb: [number, number, number]; baseline: number }> = {
  idle: { rgb: [28, 96, 120], baseline: 0.08 },
  listening: { rgb: [38, 158, 188], baseline: 0.20 },
  thinking: { rgb: [196, 138, 44], baseline: 0.26 },
  speaking: { rgb: [46, 178, 108], baseline: 0.22 },
};
const HONEY: [number, number, number] = [255, 182, 64];  // bee tint, like the GTK mix

interface Particle { a: number; rf: number; speed: number; size: number; }

export class VoiceSphere {
  private readonly canvas: HTMLCanvasElement;
  private readonly ctx: CanvasRenderingContext2D;
  private readonly reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  private raf = 0;
  private phase: SpherePhase = 'idle';
  private level = 0;
  private audio: AudioContext | null = null;
  private playbackAnalyser: AnalyserNode | null = null;
  private micAnalyser: AnalyserNode | null = null;
  private readonly elementSources = new WeakMap<HTMLMediaElement, MediaElementAudioSourceNode>();
  private freq = new Uint8Array(256);
  private time = new Uint8Array(2048);
  private smooth = new Float32Array(BINS);
  private particles: Particle[] = [];
  private pulses: number[] = [];
  private lastPulse = -10;
  private shots: { t0: number; a: number; rf: number; dir: number }[] = [];
  private bgColor = 'rgb(20,22,28)';
  private panel: HTMLElement | null = null;
  private bgTick = 0;
  private cssW = 210;
  private cssH = 210;
  private bandTop = 0;     // blob band between identity and controls (css px)
  private bandBottom = 0;

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d')!;
    for (let i = 0; i < PARTICLES; ++i) {
      this.particles.push({ a: Math.random() * TAU, rf: 0.85 + Math.random() * 0.8, speed: 0.5 + Math.random() * 0.9, size: 0.8 + Math.random() * 1.6 });
    }
    this.resize();
    const observer = new ResizeObserver(() => this.resize());
    observer.observe(canvas);
    const panel = canvas.parentElement;
    if (panel) {
      observer.observe(panel);
      this.panel = panel;
      this.refreshBg();
    }
    for (const id of ['voice-status', 'voice-controls', 'voice-health']) {
      const el = document.getElementById(id);
      if (el) observer.observe(el);
    }
    if (this.reducedMotion) this.render(0);
  }

  setPhase(phase: SpherePhase): void {
    this.phase = phase;
    if (this.reducedMotion) { this.render(0); return; }
    if (!this.raf) this.raf = requestAnimationFrame((t) => this.frame(t));
  }

  attachPlayback(media: HTMLMediaElement): void {
    try {
      this.ensureAudio();
      if (!this.elementSources.has(media)) {
        const source = this.audio!.createMediaElementSource(media);
        const analyser = this.audio!.createAnalyser();
        analyser.fftSize = 512;
        analyser.smoothingTimeConstant = 0.75;
        source.connect(analyser);
        analyser.connect(this.audio!.destination);
        this.elementSources.set(media, source);
        this.playbackAnalyser = analyser;
      }
      void this.audio!.resume();
    } catch { /* decorative; never break playback */ }
  }

  attachMic(stream: MediaStream): void {
    try {
      this.ensureAudio();
      const source = this.audio!.createMediaStreamSource(stream);
      const analyser = this.audio!.createAnalyser();
      analyser.fftSize = 512;
      analyser.smoothingTimeConstant = 0.7;
      source.connect(analyser);
      this.micAnalyser = analyser;
      void this.audio!.resume();
    } catch { /* decorative */ }
  }

  private ensureAudio(): void { this.audio ??= new AudioContext(); }

  private resize(): void {
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const rect = this.canvas.getBoundingClientRect();
    this.cssW = Math.max(1, rect.width);
    this.cssH = Math.max(1, rect.height);
    this.canvas.width = Math.max(1, Math.round(rect.width * dpr));
    this.canvas.height = Math.max(1, Math.round(rect.height * dpr));
    this.measureBand();
    if (this.reducedMotion) this.render(0);
  }

  // Vertical band between the identity block and the controls — the blob
  // lives there, so it never hides behind the text or the buttons.
  private measureBand(): void {
    const panel = this.canvas.parentElement?.getBoundingClientRect();
    const status = document.getElementById('voice-status')?.getBoundingClientRect();
    const controls = document.getElementById('voice-controls')?.getBoundingClientRect();
    if (panel && status && controls && controls.bottom > 0) {
      this.bandTop = status.bottom - panel.top;
      this.bandBottom = controls.top - panel.top;
    } else {
      this.bandTop = this.cssH * 0.25;
      this.bandBottom = this.cssH * 0.75;
    }
  }

  private activeAnalyser(): AnalyserNode | null {
    if (this.phase === 'listening') return this.micAnalyser;
    return this.playbackAnalyser;
  }

  private sampleLevel(analyser: AnalyserNode): number {
    analyser.getByteTimeDomainData(this.time);
    let sumSq = 0;
    for (let i = 0; i < analyser.fftSize; ++i) {
      const v = (this.time[i]! - 128) / 128;
      sumSq += v * v;
    }
    return Math.min(1, Math.sqrt(sumSq / analyser.fftSize) * 3.2);
  }

  private frame(t: number): void {
    this.render(t / 1000);
    if (this.phase !== 'idle' || this.level > 0.02) {
      this.raf = requestAnimationFrame((n) => this.frame(n));
    } else {
      this.raf = 0;
      this.render(t / 1000);
    }
  }

  private dpr(): number { return this.canvas.width / this.cssW; }

  private render(now: number): void {
    if ((this.bgTick++ & 31) === 0) this.refreshBg();
    const { ctx } = this;
    const dpr = this.dpr();
    const w = this.cssW;
    const h = this.cssH;
    const style = PHASE_STYLE[this.phase];
    const analyser = this.activeAnalyser();
    let instant = style.baseline;
    if (analyser) {
      analyser.getByteFrequencyData(this.freq);
      instant = Math.max(style.baseline, this.sampleLevel(analyser));
    }
    this.level += (instant - this.level) * 0.28;
    const level = this.level;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    this.drawHoneycomb(now, level, w, h);

    const cx = w / 2;
    const cy = Math.max(this.bandTop + 60, Math.min((this.bandTop + this.bandBottom) / 2, this.bandBottom - 60));
    const baseR = Math.max(52, Math.min(Math.min(w * 0.30, (this.bandBottom - this.bandTop) * 0.27), 110));

    this.knockOut(cx, cy, baseR, w, h);
    this.drawBlob(now, level, cx, cy, baseR, analyser);
    this.drawSwarm(now, level, cx, cy, baseR);
  }

  // ==== Honeycomb backdrop — direct port of the GTK fragment shader: slow
  // drift, per-cell hash brightness (twinkle pass), amber lines. ====
  private drawHoneycomb(now: number, level: number, w: number, h: number): void {
    const { ctx } = this;
    const R = 17;                                   // hex circumradius (css px)
    const stepX = 1.5 * R;
    const stepY = Math.sqrt(3) * R;
    const driftX = (now * 0.34) % (stepX * 2);
    const driftY = (now * 0.20) % (stepY * 2);
    const cols = Math.ceil(w / stepX) + 3;
    const rows = Math.ceil(h / stepY) + 3;
    const base = (0.085 + 0.10 * level) * (w / 340 + 0.5);
    for (const pass of [0, 1]) {
      ctx.beginPath();
      for (let c = -1; c < cols; ++c) {
        for (let r = -1; r < rows; ++r) {
          const x = c * stepX - driftX;
          const y = r * stepY + (c % 2 ? stepY / 2 : 0) - driftY;
          const hash = this.hash2(c + Math.floor(driftX / (stepX * 2)) * 7, r + Math.floor(driftY / (stepY * 2)) * 13);
          if (pass === 0 ? hash > 0.8 : hash <= 0.8) continue;
          this.hexPath(x, y, R * 0.94);
        }
      }
      ctx.strokeStyle = `rgba(${HONEY[0]},${HONEY[1]},${HONEY[2]},${(pass === 0 ? base : base * 1.7 * 0.6).toFixed(3)})`;
      ctx.lineWidth = 1;
      ctx.stroke();
    }
  }

  private hexPath(x: number, y: number, R: number): void {
    const { ctx } = this;
    for (let k = 0; k < 6; ++k) {
      const a = (k / 6) * TAU;
      const px = x + Math.cos(a) * R;
      const py = y + Math.sin(a) * R;
      if (k === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.closePath();
  }

  private hash2(x: number, y: number): number {
    const s = Math.sin(x * 127.1 + y * 311.7) * 43758.5453;
    return s - Math.floor(s);
  }

  // Vignette + void behind the blob: fade the hexes at the panel edges and
  // nearly erase them behind the sphere, like the GTK voidMask.
  private knockOut(cx: number, cy: number, baseR: number, w: number, h: number): void {
    const { ctx } = this;
    const edge = ctx.createRadialGradient(w / 2, h / 2, Math.min(w, h) * 0.35, w / 2, h / 2, Math.max(w, h) * 0.75);
    edge.addColorStop(0, 'rgba(0,0,0,0)');
    edge.addColorStop(1, this.fadeColor(0.30));
    ctx.fillStyle = edge;
    ctx.fillRect(0, 0, w, h);
    const voidGrad = ctx.createRadialGradient(cx, cy, baseR * 0.5, cx, cy, baseR * 1.45);
    voidGrad.addColorStop(0, this.fadeColor(0.85));
    voidGrad.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.fillStyle = voidGrad;
    ctx.fillRect(0, 0, w, h);
  }

  // Re-read the panel background periodically: the one-shot constructor read
  // could capture a light theme (or pre-theme) color and paint white fade
  // discs over a dark panel forever after. Never trust a stale snapshot.
  private refreshBg(): void {
    if (!this.panel) return;
    const c = getComputedStyle(this.panel).backgroundColor;
    if (c) this.bgColor = c;
  }

  private fadeColor(alpha: number): string {
    // Luminance guard: on a dark desktop a "fade to background" must never be
    // light — if the captured color is bright or unparseable, use near-black.
    const m = this.bgColor.match(/(\d+)[^\d]+(\d+)[^\d]+(\d+)/);
    const lum = m ? (0.299 * Number(m[1]) + 0.587 * Number(m[2]) + 0.114 * Number(m[3])) / 255 : 1;
    if (!m || lum > 0.55) return `rgba(10,12,16,${alpha})`;
    return this.bgColor.startsWith('rgb') && !this.bgColor.startsWith('rgba')
      ? this.bgColor.replace('rgb(', 'rgba(').replace(')', `,${alpha})`)
      : `rgba(10,12,16,${alpha})`;
  }

  // ==== Waveform when audio is live; living nebula orb when idle. ====
  private drawBlob(now: number, level: number, cx: number, cy: number, baseR: number, analyser: AnalyserNode | null): void {
    const { ctx } = this;
    const style = PHASE_STYLE[this.phase];
    const thinking = this.phase === 'thinking';
    for (let i = 0; i < BINS; ++i) {
      let target: number;
      if (analyser) {
        const b = Math.min(this.freq.length - 1, 2 + Math.floor(Math.pow(i / BINS, 1.3) * 58));
        target = Math.min(1, (this.freq[b]! / 255) * 1.6);
      } else {
        target = style.baseline * (0.75 + 0.45 * Math.sin(now * 0.8 + i * 0.62)) + 0.05 * Math.sin(now * 1.9 + i * 1.7) + (thinking ? 0.08 * (0.5 + 0.5 * Math.sin(now * 3.4 + i * 0.4)) : 0);
      }
      this.smooth[i] = this.smooth[i]! + (target - this.smooth[i]!) * 0.45;
    }

    ctx.save();
    ctx.translate(cx, cy);
    ctx.lineCap = 'round';

    if (analyser) {
      // ---- Live audio: mirrored radial ray waveform (the part you liked). ----
      const spin = now * (0.10 + level * 0.35 + (thinking ? 0.4 : 0));
      const inner = baseR * 0.38;
      const maxLen = baseR * 0.85;
      const core = ctx.createRadialGradient(0, 0, 0, 0, 0, inner * 1.25);
      core.addColorStop(0, 'rgba(4,6,9,0.95)');
      core.addColorStop(0.8, this.fadeColor(0.85));
      core.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.beginPath();
      ctx.arc(0, 0, inner * 1.25, 0, TAU);
      ctx.fillStyle = core;
      ctx.fill();
      ctx.beginPath();
      ctx.arc(0, 0, inner, 0, TAU);
      ctx.strokeStyle = this.rgba(style.rgb, 0.12 + level * 0.10);
      ctx.lineWidth = 1;
      ctx.stroke();
      for (let i = 0; i < RAYS; ++i) {
        const bi = Math.min(BINS - 1, Math.floor((i / RAYS) * BINS));
        const amp = Math.max(this.smooth[bi]!, this.smooth[BINS - 1 - bi]! * 0.85);
        const a = (i / RAYS) * TAU + spin;
        const len = baseR * 0.08 + amp * maxLen;
        const hot = Math.min(1, amp * 1.4) * 0.7;
        const rgb = style.rgb.map((c, k) => Math.round(c + (HONEY[k]! * 0.82 - c) * hot));
        ctx.beginPath();
        ctx.moveTo(Math.cos(a) * inner, Math.sin(a) * inner);
        ctx.lineTo(Math.cos(a) * (inner + len), Math.sin(a) * (inner + len));
        ctx.strokeStyle = this.rgba(rgb, 0.16 + amp * 0.42);
        ctx.lineWidth = 1.8;
        ctx.stroke();
      }
      ctx.restore();
      return;
    }

    // ---- Idle / thinking: the living nebula orb. Noise-driven liquid edge,
    // nested contour rings, heartbeat pulses, shooting stars. Never repeats. ----
    const R0 = baseR * 0.78;
    const t = now * (thinking ? 1.8 : 1.0);
    const spin = now * (thinking ? 0.5 : 0.10);

    const bg = ctx.createRadialGradient(0, 0, 0, 0, 0, R0 * 1.4);
    bg.addColorStop(0, this.fadeColor(0.9));
    bg.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.beginPath();
    ctx.arc(0, 0, R0 * 1.4, 0, TAU);
    ctx.fillStyle = bg;
    ctx.fill();

    const layers = [1, 0.74, 0.5];
    for (let li = 0; li < layers.length; ++li) {
      const L = layers[li]!;
      ctx.beginPath();
      for (let i = 0; i <= 72; ++i) {
        const th = (i / 72) * TAU + spin;
        const n = fbm(Math.cos(th) * 1.7 + t * 0.22, Math.sin(th) * 1.7 - t * 0.16);
        const breathe = 1 + 0.05 * Math.sin(t * 0.9 + li * 2.1);
        const r = R0 * L * breathe * (1 + 0.20 * n) + level * baseR * 0.1;
        const x = Math.cos(th) * r, y = Math.sin(th) * r;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.closePath();
      const rgb = style.rgb.map((c, k) => Math.round(c + (HONEY[k]! - c) * (1 - L) * 0.8));
      ctx.strokeStyle = this.rgba(rgb, (0.26 - li * 0.06) + level * 0.2);
      ctx.lineWidth = 1.3;
      ctx.stroke();
    }

    const glow = ctx.createRadialGradient(0, 0, 0, 0, 0, R0 * 1.05);
    glow.addColorStop(0, this.rgba(style.rgb, 0.10 + level * 0.12));
    glow.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.beginPath();
    ctx.arc(0, 0, R0 * 1.05, 0, TAU);
    ctx.fillStyle = glow;
    ctx.fill();

    // accretion disk around the living core
    this.drawDisk(t, level, R0, style.rgb);

    if (!this.reducedMotion) {
      if (now - this.lastPulse > (thinking ? 1.8 : 3.8) + hash2(now, 1)) {
        this.pulses.push(now);
        this.lastPulse = now;
      }
      this.pulses = this.pulses.filter((p) => now - p < 2.2);
      for (const p of this.pulses) {
        const k = (now - p) / 2.2;
        const e = 1 - Math.pow(1 - k, 3);
        ctx.beginPath();
        ctx.arc(0, 0, R0 * (1.05 + e * 1.15), 0, TAU);
        ctx.strokeStyle = this.rgba(style.rgb, (1 - k) * 0.22);
        ctx.lineWidth = 1.2;
        ctx.stroke();
      }
      if (Math.random() < 0.006 && this.shots.length < 2) {
        this.shots.push({ t0: now, a: Math.random() * TAU, rf: 1.1 + Math.random() * 0.4, dir: Math.random() < 0.5 ? 1 : -1 });
      }
      this.shots = this.shots.filter((sh) => now - sh.t0 < 1.0);
      for (const sh of this.shots) {
        const k = (now - sh.t0) / 1.0;
        const head = sh.a + sh.dir * k * 1.6;
        const r = sh.rf * R0 * (1 + k * 0.25);
        for (let q = 0; q < 10; ++q) {
          const aa = head - sh.dir * q * 0.045;
          ctx.beginPath();
          ctx.arc(Math.cos(aa) * r, Math.sin(aa) * r, Math.max(0.4, 1.4 - q * 0.1), 0, TAU);
          ctx.fillStyle = this.rgba(HONEY, (1 - k) * (1 - q / 10) * 0.55);
          ctx.fill();
        }
      }
    }
    ctx.restore();
  }

  // ==== Accretion disk: procedural logarithmic spiral arms — short arc
  // segments with noise-mottled alpha for a misty, static-y texture. Inner
  // orbits rotate faster than outer (Kepler-ish shear), so the arms visibly
  // wind inward like real accretion flow. Cool phase hue outside, hot honey
  // at the rim. ~270 strokes/frame — no particle bookkeeping. ====
  private drawDisk(t: number, level: number, R0: number, base: number[]): void {
    const { ctx } = this;
    // faint haze body of the disk
    const body = ctx.createRadialGradient(0, 0, R0 * 0.55, 0, 0, R0 * 1.4);
    body.addColorStop(0, 'rgba(0,0,0,0)');
    body.addColorStop(0.45, this.rgba(base, 0.05 + level * 0.05));
    body.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.beginPath();
    ctx.arc(0, 0, R0 * 1.4, 0, TAU);
    ctx.fillStyle = body;
    ctx.fill();

    const ARMS = 3, SEGS = 90, B = 0.28;
    for (let arm = 0; arm < ARMS; ++arm) {
      for (let k = 0; k < SEGS; ++k) {
        const f = k / SEGS; // 0 = outer edge, 1 = inner rim
        const r = R0 * (1.38 - 0.78 * f);
        const rot = t * 0.55 * (R0 / r); // inner races ahead — visible shear
        const th = arm * (TAU / ARMS) + Math.log(r / (R0 * 0.6)) / B + rot;
        const n = fbm(Math.cos(th) * 2 + t * 0.3 + arm * 7, Math.sin(th) * 2 - t * 0.2);
        const hot = 1 - f;
        const rgb = base.map((c, k2) => Math.round(c + (HONEY[k2]! - c) * hot));
        const a = (0.04 + 0.14 * hot) * (0.45 + 0.55 * n) + level * 0.08;
        ctx.beginPath();
        ctx.arc(0, 0, r, th, th + 0.15);
        ctx.strokeStyle = this.rgba(rgb, a);
        ctx.lineWidth = 0.6 + hot * 1.7;
        ctx.stroke();
      }
    }
  }

  // ==== Orbiting swarm: particles in real orbits around the horizon,
  // faster and hotter with the voice — comets around the black hole. ====
  private drawSwarm(_now: number, level: number, cx: number, cy: number, baseR: number): void {
    const { ctx } = this;
    const style = PHASE_STYLE[this.phase];
    for (const p of this.particles) {
      // Kepler-ish: inner stars race, outer drift — constant visible shear.
      p.a += 0.016 * p.speed * (1.4 / p.rf) * (0.5 + level * 1.8 + (this.phase === 'thinking' ? 0.7 : 0));
      p.rf += (Math.random() - 0.5) * 0.004;
      p.rf = Math.max(0.85, Math.min(1.65, p.rf));
    }
    const pos = this.particles.map((p) => ({ x: Math.cos(p.a) * p.rf * baseR, y: Math.sin(p.a) * p.rf * baseR }));
    ctx.save();
    ctx.translate(cx, cy);
    ctx.lineWidth = 0.7;
    for (let i = 0; i < PARTICLES; ++i) {
      for (let j = i + 1; j < PARTICLES; ++j) {
        const a = pos[i]!, b = pos[j]!;
        const d = Math.hypot(a.x - b.x, a.y - b.y);
        if (d < baseR * 0.35) {
          ctx.beginPath();
          ctx.moveTo(a.x, a.y);
          ctx.lineTo(b.x, b.y);
          ctx.strokeStyle = this.rgba(style.rgb, (1 - d / (baseR * 0.35)) * (0.04 + level * 0.08));
          ctx.stroke();
        }
      }
    }
    for (let i = 0; i < PARTICLES; ++i) {
      const p = this.particles[i]!;
      ctx.beginPath();
      ctx.arc(pos[i]!.x, pos[i]!.y, p.size * (0.8 + level * 0.6), 0, TAU);
      const tw = 0.5 + 0.5 * Math.sin(_now * 2.1 + i * 1.93);
      ctx.fillStyle = this.rgba(HONEY, 0.28 + tw * 0.42 + level * 0.30);
      ctx.fill();
    }
    ctx.restore();
  }

  private rgba(rgb: readonly number[], alpha: number): string {
    return `rgba(${rgb[0]},${rgb[1]},${rgb[2]},${Math.min(1, alpha).toFixed(3)})`;
  }
}

// Single instance shared by the voice controller (phase/mic) and the speech
// player (TTS playback analysis). The canvas fills the voice panel.
export const voiceSphere = new VoiceSphere(byId<HTMLCanvasElement>('voice-sphere'));
