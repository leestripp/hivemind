// js/pages/system.js — per-worker machine telemetry + agent usage.
// Workers sample their own hosts and attach metrics to the existing 15 s
// heartbeat. The hub retains each worker's history and serves this page;
// polling happens ONLY while the page is active (leave() clears the timer).
import { $, esc } from '../util.js';
import { API } from '../api.js';

let timer = null;
let histMinutes = 60;
let refreshing = false;

function number(value, fallback = 0) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function fmtUptime(value) {
  const s = number(value);
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600),
        m = Math.floor((s % 3600) / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`;
}

const pct = value => `${Math.round(number(value) * 10) / 10}%`;
const barClass = value => number(value) >= 90 ? 'crit' : number(value) >= 75 ? 'warn' : '';

function renderCards(system, processLabel) {
  const n = system.now || {}, h = system.host || {};
  const rss = n.worker_rss_mb ?? n.server_rss_mb ?? n.rss_mb;
  return `
    <div class="sys-card"><div class="sys-label">Host</div>
      <div class="sys-value">${esc(h.hostname || 'unknown')}</div>
      <div class="sys-sub">${number(h.cores)} cores · kernel ${esc(h.kernel || 'unknown')} · up ${fmtUptime(h.uptime_s)}</div></div>
    <div class="sys-card"><div class="sys-label">Memory</div>
      <div class="sys-value ${barClass(n.mem_pct)}">${pct(n.mem_pct)}</div>
      <div class="sys-sub">${number(n.mem_used_mb)} / ${number(n.mem_total_mb)} MB</div></div>
    <div class="sys-card"><div class="sys-label">CPU</div>
      <div class="sys-value ${barClass(n.cpu_pct)}">${pct(n.cpu_pct)}</div>
      <div class="sys-sub">load ${number(n.load1).toFixed(2)}</div></div>
    <div class="sys-card"><div class="sys-label">Disk /</div>
      <div class="sys-value ${barClass(n.disk_pct)}">${pct(n.disk_pct)}</div>
      <div class="sys-sub">${number(n.disk_used_gb)} / ${number(n.disk_total_gb)} GB</div></div>
    <div class="sys-card"><div class="sys-label">${esc(processLabel)} RSS</div>
      <div class="sys-value">${number(rss)} MB</div>
      <div class="sys-sub">${number(n.containers)} agent container${number(n.containers) === 1 ? '' : 's'} running</div></div>`;
}

// Minimal canvas line chart: series = [{t, v}] scaled 0..maxY.
function drawChart(canvas, series, maxY, color, windowMinutes, emptyMessage) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (!w || !h) return;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const css = getComputedStyle(document.documentElement);
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = css.getPropertyValue('--border') || '#333';
  ctx.lineWidth = 1;
  for (const f of [0.25, 0.5, 0.75]) {
    ctx.beginPath(); ctx.moveTo(0, h * f); ctx.lineTo(w, h * f); ctx.stroke();
  }
  if (series.length < 2) {
    ctx.fillStyle = css.getPropertyValue('--muted') || '#888';
    ctx.font = '12px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText(emptyMessage, w / 2, h / 2);
    return;
  }
  // Use the requested time window even while a newly started hub has only a
  // few samples; otherwise all three range buttons render identically.
  const t1 = Math.floor(Date.now() / 1000);
  const t0 = t1 - windowMinutes * 60;
  const span = Math.max(1, t1 - t0);
  ctx.strokeStyle = color; ctx.lineWidth = 1.6; ctx.beginPath();
  series.forEach((p, i) => {
    const x = (p.t - t0) / span * w;
    const y = h - Math.min(1, Math.max(0, p.v / maxY)) * h;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
  ctx.fillStyle = css.getPropertyValue('--muted') || '#888';
  ctx.font = '10px sans-serif';
  const lbl = t => new Date(t * 1000).toISOString().slice(11, 16);
  ctx.textAlign = 'left'; ctx.fillText(`${lbl(t0)} UTC`, 4, h - 4);
  ctx.textAlign = 'right'; ctx.fillText(`${lbl(t1)} UTC`, w - 4, h - 4);
}

function drawGraphs(section, history) {
  if (!section || !history) return;
  const cutoff = Math.floor(Date.now() / 1000) - histMinutes * 60;
  const samples = (Array.isArray(history.samples) ? history.samples : [])
    .filter(p => number(p.t) >= cutoff);
  const css = getComputedStyle(document.documentElement);
  const empty = history.error ? 'history unavailable' : 'collecting samples…';
  const metrics = [
    ['mem', css.getPropertyValue('--accent') || '#88c0d0'],
    ['cpu', '#a3be8c'],
    ['disk', '#d8a657'],
  ];
  for (const [metric, color] of metrics) {
    const canvas = section.querySelector(`canvas[data-metric="${metric}"]`);
    if (canvas) drawChart(canvas,
      samples.map(p => ({ t: number(p.t), v: number(p[metric]) })),
      100, color, histMinutes, empty);
  }
}

function graphBoxes() {
  return `<div class="sys-graphs">
    ${[['mem', 'Memory %'], ['cpu', 'CPU %'], ['disk', 'Disk %']].map(
      ([metric, title]) => `<div class="sys-graphbox">
        <div class="sys-graphtitle">${title}</div>
        <canvas class="sys-graph" data-metric="${metric}" aria-label="${title} history"></canvas>
      </div>`).join('')}
  </div>`;
}

function renderWaiting(worker, index) {
  return `<section class="sys-worker" data-sys-index="${index}">
    <div class="sys-worker-head"><h3>${esc(worker.name)}
      <span class="sys-worker-state ${worker.online ? 'online' : 'offline'}">${worker.online ? 'online' : 'offline'}</span></h3></div>
    <div class="sys-cards"><div class="sys-card"><div class="sys-value">awaiting telemetry</div>
      <div class="sys-sub">${worker.online ?
        'The worker is connected; its first host sample should arrive within 15 seconds. An older worker must be upgraded to report host stats.' :
        'The worker is offline. Host stats resume after it reconnects.'}</div></div></div>
  </section>`;
}

function renderSystem(system, index, title, processLabel, history) {
  return `<section class="sys-worker" data-sys-index="${index}">
    <div class="sys-worker-head"><h3>${esc(title)}
      ${system.online === undefined ? '' : `<span class="sys-worker-state ${system.online ? 'online' : 'offline'}">${system.online ? 'online' : 'offline'}</span>`}</h3>
      ${history?.error ? `<span class="sys-history-error">${esc(history.error)}</span>` : ''}</div>
    <div class="sys-cards">${renderCards(system, processLabel)}</div>
    ${graphBoxes()}
  </section>`;
}

function renderSystems(snapshot, histories) {
  const root = $('sys-workers');
  const workers = Array.isArray(snapshot.workers) ? snapshot.workers : [];
  if (!workers.length) {
    root.innerHTML = renderSystem(snapshot, 0, 'Hub host', 'hive-server', histories.get(''));
    drawGraphs(root.querySelector('[data-sys-index="0"]'), histories.get(''));
    return;
  }
  root.innerHTML = workers.map((worker, index) => worker.telemetry ?
    renderSystem(worker, index, worker.name, 'hive-worker', histories.get(worker.name)) :
    renderWaiting(worker, index)).join('');
  workers.forEach((worker, index) => {
    if (worker.telemetry) {
      drawGraphs(root.querySelector(`[data-sys-index="${index}"]`), histories.get(worker.name));
    }
  });
}

function renderAgents(d) {
  const rows = (d.agents || []).map(a => `
    <tr><td>${esc(a.name)}</td><td>${esc(a.worker)}</td>
        <td>${a.execs_24h}</td><td>${a.exec_timeouts_24h}</td>
        <td>${a.exec_avg_ms_24h}</td><td>${a.messages_sent_24h}</td>
        <td>${a.last_exec ? esc(a.last_exec.slice(5, 16).replace('T', ' ') + ' UTC') : '—'}</td></tr>`).join('');
  $('sys-agents-body').innerHTML = rows ||
    '<tr><td colspan="7" class="muted">no agents</td></tr>';
  $('sys-agents-sub').textContent =
    `last 24 h · ${d.user_messages_24h} user messages in the same window`;
}

async function getHistory(workerName) {
  try {
    const suffix = workerName ? `&worker=${encodeURIComponent(workerName)}` : '';
    const history = await API.get(`/api/system/history?minutes=${histMinutes}${suffix}`);
    return [workerName, history];
  } catch (error) {
    return [workerName, { samples: [], error: error.message }];
  }
}

async function refreshAll() {
  if (refreshing) return;
  refreshing = true;
  try {
    const [snapshot, agents] = await Promise.all([
      API.get('/api/system'),
      API.get('/api/system/agents'),
    ]);
    const workers = Array.isArray(snapshot.workers) ? snapshot.workers : [];
    const targets = workers.length ? workers.filter(w => w.telemetry).map(w => w.name) : [''];
    const histories = new Map(await Promise.all(targets.map(getHistory)));
    renderSystems(snapshot, histories);
    renderAgents(agents);
  } catch (error) {
    console.error('System refresh failed', error);
    $('sys-workers').innerHTML =
      `<div class="sys-card"><div class="sys-value crit">unavailable</div>
       <div class="sys-sub">${esc(error.message)}</div></div>`;
  } finally {
    refreshing = false;
  }
}

function requestRefresh() {
  refreshAll().catch(error => console.error('System refresh failed', error));
}

export function initSystem() {
  document.querySelectorAll('.sys-range').forEach(button =>
    button.addEventListener('click', () => {
      document.querySelectorAll('.sys-range').forEach(x => x.classList.remove('active'));
      button.classList.add('active');
      histMinutes = parseInt(button.dataset.minutes, 10);
      requestRefresh();
    }));
}

export function enterSystem() {
  requestRefresh();
  if (timer) clearInterval(timer);
  timer = setInterval(requestRefresh, 10000);
}

export function leaveSystem() {
  if (timer) { clearInterval(timer); timer = null; }
}
