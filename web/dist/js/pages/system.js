// js/pages/system.js — System page: host metrics + agent usage.
// Polls /api/system* ONLY while the page is active (leave() clears the
// timer). The server's SystemMonitor samples regardless (15 s, in-memory),
// so graphs have history even on first open.
import { $, esc } from '../util.js';
import { API } from '../api.js';

let timer = null;
let histMinutes = 60;
let lastHist = null;   // cached series so 5 s ticks redraw without refetch

function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600),
        m = Math.floor((s % 3600) / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`;
}
const pct = v => (Math.round(v * 10) / 10) + '%';

function barClass(v) { return v >= 90 ? 'crit' : v >= 75 ? 'warn' : ''; }

function renderCards(d) {
  const n = d.now, h = d.host;
  $('sys-cards').innerHTML = `
    <div class="sys-card"><div class="sys-label">Host</div>
      <div class="sys-value">${esc(h.hostname)}</div>
      <div class="sys-sub">${h.cores} cores · kernel ${esc(h.kernel)} · up ${fmtUptime(h.uptime_s)}</div></div>
    <div class="sys-card"><div class="sys-label">Memory</div>
      <div class="sys-value ${barClass(n.mem_pct)}">${pct(n.mem_pct)}</div>
      <div class="sys-sub">${n.mem_used_mb} / ${n.mem_total_mb} MB</div></div>
    <div class="sys-card"><div class="sys-label">CPU</div>
      <div class="sys-value ${barClass(n.cpu_pct)}">${pct(n.cpu_pct)}</div>
      <div class="sys-sub">load ${n.load1.toFixed(2)}</div></div>
    <div class="sys-card"><div class="sys-label">Disk /</div>
      <div class="sys-value ${barClass(n.disk_pct)}">${pct(n.disk_pct)}</div>
      <div class="sys-sub">${n.disk_used_gb} / ${n.disk_total_gb} GB</div></div>
    <div class="sys-card"><div class="sys-label">hive-server RSS</div>
      <div class="sys-value">${n.server_rss_mb} MB</div>
      <div class="sys-sub">${n.containers} agent container${n.containers === 1 ? '' : 's'} running</div></div>`;
}

// Minimal canvas line chart: series = [{t, v}] scaled 0..maxY.
function drawChart(canvas, series, maxY, color) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (!w || !h) return;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const css = getComputedStyle(document.documentElement);
  ctx.clearRect(0, 0, w, h);
  // gridlines at 25/50/75%
  ctx.strokeStyle = css.getPropertyValue('--border') || '#333';
  ctx.lineWidth = 1;
  for (const f of [0.25, 0.5, 0.75]) {
    ctx.beginPath(); ctx.moveTo(0, h * f); ctx.lineTo(w, h * f); ctx.stroke();
  }
  if (series.length < 2) {
    ctx.fillStyle = css.getPropertyValue('--muted') || '#888';
    ctx.font = '12px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText('collecting samples…', w / 2, h / 2);
    return;
  }
  const t0 = series[0].t, t1 = series[series.length - 1].t, span = Math.max(1, t1 - t0);
  ctx.strokeStyle = color; ctx.lineWidth = 1.6; ctx.beginPath();
  series.forEach((p, i) => {
    const x = (p.t - t0) / span * w;
    const y = h - Math.min(1, p.v / maxY) * h;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
  // time axis labels: first/last sample times (UTC HH:MM)
  ctx.fillStyle = css.getPropertyValue('--muted') || '#888';
  ctx.font = '10px sans-serif';
  const lbl = t => new Date(t * 1000).toISOString().slice(11, 16);
  ctx.textAlign = 'left';  ctx.fillText(lbl(t0) + ' UTC', 4, h - 4);
  ctx.textAlign = 'right'; ctx.fillText(lbl(t1) + ' UTC', w - 4, h - 4);
}

function drawGraphs() {
  if (!lastHist) return;
  const s = lastHist.samples || [];
  const css = getComputedStyle(document.documentElement);
  drawChart($('sys-mem-graph'), s.map(p => ({ t: p.t, v: p.mem })), 100, css.getPropertyValue('--accent') || '#88c0d0');
  drawChart($('sys-cpu-graph'), s.map(p => ({ t: p.t, v: p.cpu })), 100, '#a3be8c');
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

async function refreshAll() {
  try {
    const [snap, hist, ag] = await Promise.all([
      API.get('/api/system'),
      API.get(`/api/system/history?minutes=${histMinutes}`),
      API.get('/api/system/agents'),
    ]);
    lastHist = hist;
    renderCards(snap);
    drawGraphs();
    renderAgents(ag);
  } catch (e) {
    $('sys-cards').innerHTML =
      `<div class="sys-card"><div class="sys-value crit">unavailable</div>
       <div class="sys-sub">${esc(e.message)}</div></div>`;
  }
}

export function initSystem() {
  document.querySelectorAll('.sys-range').forEach(b =>
    b.addEventListener('click', () => {
      document.querySelectorAll('.sys-range').forEach(x => x.classList.remove('active'));
      b.classList.add('active');
      histMinutes = parseInt(b.dataset.minutes, 10);
      refreshAll();
    }));
}

export function enterSystem() {
  refreshAll();
  timer = setInterval(refreshAll, 10000);
}

export function leaveSystem() {
  if (timer) { clearInterval(timer); timer = null; }
}
