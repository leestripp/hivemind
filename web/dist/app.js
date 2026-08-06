// =============================================================================
// HiveMind Node Graph — app.js
// Pure-vanilla, self-contained node graph editor + runner simulator.
// =============================================================================

const NODE_DEFS = {
  input:       { color: '#a3be8c', label: 'Input',       in: [],        out: ['out'] },
  prompt:      { color: '#b48ead', label: 'Prompt',      in: ['in'],     out: ['out'] },
  agent:       { color: '#88c0d0', label: 'AI Agent',    in: ['in'],     out: ['out'] },
  merge:       { color: '#ebcb8b', label: 'Merge',       in: ['in_a','in_b','in_c'], out: ['out'] },
  output:      { color: '#d08770', label: 'Output',      in: ['in'],     out: [] },
  log:         { color: '#6c7280', label: 'Log',         in: ['in'],     out: [] },
  loop_start:  { color: '#5e81ac', label: 'Loop Start',  in: ['in'],     out: ['out','loop_count'] },
  loop_end:    { color: '#5e81ac', label: 'Loop End',    in: ['in','loop_count'], out: ['out'] },
};

const LOOP_PAIR_SPACING = 220; // px between loop_start and loop_end

const NODE_WIDTH = 220;

const S = {
  nodes: [],
  edges: [],
  selected: null,
  dragging: null,
  connecting: null,
  running: false,
  // Canvas transform (pan/zoom). Node positions are stored in canvas coordinates.
  scale: 1,
  panX: 0,
  panY: 0,
  panning: null,
  spaceHeld: false,
  menuPos: null,
};

// ─── Coordinate helpers ─────────────────────────────────────────────────
// The .nodes/.edges layers are CSS-transformed with translate(panX,panY) scale(scale).
// Node (x,y) are canvas coordinates. These helpers convert screen ↔ canvas.
function canvasWrapRect() {
  return document.querySelector('.canvas-wrap').getBoundingClientRect();
}
function screenToCanvas(sx, sy) {
  const r = canvasWrapRect();
  return {
    x: (sx - r.left - S.panX) / S.scale,
    y: (sy - r.top - S.panY) / S.scale,
  };
}
function canvasToScreen(cx, cy) {
  const r = canvasWrapRect();
  return {
    x: r.left + S.panX + cx * S.scale,
    y: r.top + S.panY + cy * S.scale,
  };
}
function resetView() {
  S.scale = 1; S.panX = 0; S.panY = 0;
}

// ─── Sample graphs ───────────────────────────────────────────────────────
function loadSampleGraph() {
  // Default: a tiny loop demo that exercises the new Loop node pair.
  // (Use the palette to create a fresh Loop pair on an empty canvas.)
  const id = (t) => `n-${t}`;
  const mk = (type, label, x, y, config = {}) => ({
    id: id(label), type, label, x, y, config,
  });
  S.nodes = [
    mk('input',      'topic',       40, 140, { param_name: 'topic', value: 'a' }),
    mk('loop_start', 'start',      240, 140, { loop_count: 3 }),
    mk('prompt',     'append',     480, 140, { value: 'x' }),
    mk('loop_end',   'end',        720, 140, {}),
    mk('output',     'result',     960, 140, { name: 'result' }),
  ];
  const e = (s, sp, t, tp) => ({
    id: `e-${s}-${sp}-${t}-${tp}`,
    source: id(s), sourcePort: sp, target: id(t), targetPort: tp,
  });
  S.edges = [
    // Upstream text into the loop
    e('topic',  'out',        'start',  'in'),
    // Body of the loop
    e('start',  'out',        'append', 'in'),
    e('append', 'out',        'end',    'in'),
    // Permanent loop_count connection (start → end)
    e('start',  'loop_count', 'end',    'loop_count'),
    // Final output after the loop
    e('end',    'out',        'result', 'in'),
  ];
}

function loadMultiAgentGraph() {
  // The original multi-agent demo (still runnable).
  const id = (t) => `n-${t}`;
  const mk = (type, label, x, y, config = {}) => ({
    id: id(label), type, label, x, y, config,
  });
  S.nodes = [
    mk('input',  'topic',       40, 100, { param_name: 'topic', value: 'Should we use SQLite or Postgres?' }),
    mk('prompt', 'ask-pros',    40, 240, { value: 'Give a 2-line verdict on SQLite vs Postgres.' }),
    mk('agent',  'sqlite-pro',  280, 60,  { model: 'minimax', project: '~/work/sqlite' }),
    mk('agent',  'postgres-pro',280, 220, { model: 'minimax', project: '~/work/postgres' }),
    mk('merge',  'merge',       520, 140, { strategy: 'concat' }),
    mk('agent',  'synthesiser', 760, 140, { model: 'minimax' }),
    mk('output', 'verdict',     1000,140, { name: 'verdict' }),
  ];
  const e = (s, sp, t, tp) => ({
    id: `e-${s}-${sp}-${t}-${tp}`,
    source: id(s), sourcePort: sp, target: id(t), targetPort: tp,
  });
  S.edges = [
    e('topic',          'out', 'ask-pros',      'in'),
    e('ask-pros',       'out', 'sqlite-pro',   'in'),
    e('ask-pros',       'out', 'postgres-pro', 'in'),
    e('sqlite-pro',     'out', 'merge',        'in_a'),
    e('postgres-pro',   'out', 'merge',        'in_b'),
    e('merge',          'out', 'synthesiser',  'in'),
    e('synthesiser',    'out', 'verdict',      'in'),
  ];
}

// ─── Render ─────────────────────────────────────────────────────────────
const $ = (id) => document.getElementById(id);

// Return the exact canvas coordinates for the centre of a port dot.
// We read the DOM after render() so the edge always lines up with the real
// port position, regardless of CSS tweaks, font metrics, or node height.
function portCenter(node, portName, dir) {
  const el = document.querySelector(`.node[data-id="${CSS.escape(node.id)}"] .port[data-port="${CSS.escape(portName)}"][data-dir="${dir}"]`);
  if (!el) {
    // Fallback: centre of the node edge on the appropriate side.
    return { x: dir === 'output' ? node.x + NODE_WIDTH : node.x, y: node.y + 40 };
  }
  const rect = el.getBoundingClientRect();
  const wrap = canvasWrapRect();
  return {
    x: (rect.left + rect.width / 2 - wrap.left - S.panX) / S.scale,
    y: (rect.top + rect.height / 2 - wrap.top - S.panY) / S.scale,
  };
}

function render() {
  // Mark ports that participate in an edge so we can style them as connected.
  const connectedPorts = new Set();
  for (const e of S.edges) {
    connectedPorts.add(`${e.source}:${e.sourcePort}:output`);
    connectedPorts.add(`${e.target}:${e.targetPort}:input`);
  }

  // Apply the shared pan/zoom transform to both layers.
  const transform = `translate(${S.panX.toFixed(1)}px, ${S.panY.toFixed(1)}px) scale(${S.scale.toFixed(4)})`;
  $('nodes').style.transform = transform;
  $('nodes').style.transformOrigin = '0 0';
  $('edges').style.transform = transform;
  $('edges').style.transformOrigin = '0 0';

  // edges
  const svg = $('edges');
  svg.innerHTML = '';
  for (const edge of S.edges) {
    const src = S.nodes.find(n => n.id === edge.source);
    const dst = S.nodes.find(n => n.id === edge.target);
    if (!src || !dst) continue;
    const srcPt = portCenter(src, edge.sourcePort, 'output');
    const dstPt = portCenter(dst, edge.targetPort, 'input');
    const dx = dstPt.x - srcPt.x;
    // Control points give a clean S-curve and keep the wire clear of the source/target bodies.
    const cp1x = srcPt.x + Math.max(40, Math.abs(dx) * 0.45);
    const cp2x = dstPt.x - Math.max(40, Math.abs(dx) * 0.45);
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', `M ${srcPt.x.toFixed(1)} ${srcPt.y.toFixed(1)} C ${cp1x.toFixed(1)} ${srcPt.y.toFixed(1)}, ${cp2x.toFixed(1)} ${dstPt.y.toFixed(1)}, ${dstPt.x.toFixed(1)} ${dstPt.y.toFixed(1)}`);
    path.setAttribute('class', src.running || dst.running ? 'active' : '');
    path.setAttribute('title', `${esc(edge.sourcePort)} → ${esc(edge.targetPort)}`);
    svg.appendChild(path);
  }

  // nodes
  const layer = $('nodes');
  layer.innerHTML = '';
  for (const n of S.nodes) {
    const def = NODE_DEFS[n.type];
    const div = document.createElement('div');
    div.className = 'node';
    if (S.selected === n.id) div.classList.add('selected');
    if (n.running) div.classList.add('running');
    div.style.left = n.x + 'px';
    div.style.top  = n.y + 'px';
    div.dataset.id = n.id;

    const sub = nodeSub(n);

    div.innerHTML = `
      <div class="node-header">
        <div class="node-title">
          <span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:${def.color}"></span>
          ${esc(n.label)}
          <span class="node-type">${n.type}</span>
        </div>
      </div>
      <div class="node-body">
        <div class="node-ports-in">
          ${def.in.map((port, idx) => `<div class="port-row in"><div class="port input ${connectedPorts.has(`${n.id}:${port}:input`) ? 'connected' : ''}" data-id="${n.id}" data-port="${port}" data-dir="input" data-idx="${idx}" title="${esc(port)}"></div><div class="port-label">${esc(port)}</div></div>`).join('')}
        </div>
        <div class="node-content"><div class="node-sub ${sub.muted ? 'muted' : ''}">${esc(sub.text)}</div></div>
        <div class="node-ports-out">
          ${def.out.map((port, idx) => `<div class="port-row out"><div class="port-label">${esc(port)}</div><div class="port output ${connectedPorts.has(`${n.id}:${port}:output`) ? 'connected' : ''}" data-id="${n.id}" data-port="${port}" data-dir="output" data-idx="${idx}" title="${esc(port)}"></div></div>`).join('')}
        </div>
      </div>
    `;
    layer.appendChild(div);
  }

  renderInspector();
}

function nodeSub(n) {
  if (n.type === 'input')       return { text: n.config.value || '(no value)',  muted: !n.config.value };
  if (n.type === 'prompt')      return { text: n.config.value || '(empty)',     muted: !n.config.value };
  if (n.type === 'agent')       {
    const m = n.config.model || '?';
    const p = n.config.project ? n.config.project.split('/').pop() : '';
    return { text: `${m}${p ? ' · ' + p : ''}`, muted: false };
  }
  if (n.type === 'merge')       return { text: n.config.strategy || 'concat',   muted: false };
  if (n.type === 'output')      return { text: n.config.name || 'verdict',      muted: !n.config.name };
  if (n.type === 'log')         return { text: n.config.message || '(no msg)',  muted: !n.config.message };
  if (n.type === 'loop_start')  return { text: `loops: ${n.config.loop_count ?? 1}`, muted: false };
  if (n.type === 'loop_end')    return { text: 'loop end', muted: true };
  return { text: '', muted: true };
}

function renderInspector() {
  const i = $('inspector');
  const n = S.nodes.find(x => x.id === S.selected);
  if (!n) { i.innerHTML = '<div class="panel-title">Inspector</div><p class="muted">Click a node to see its config.</p>'; return; }
  const def = NODE_DEFS[n.type];
  i.innerHTML = `
    <div class="panel-title">Inspector</div>
    <div class="node-card">
      <div class="node-card-title">${esc(n.label)}</div>
      <div class="node-card-type">${n.type} · id ${esc(n.id)}</div>
      <pre>${esc(JSON.stringify(n.config, null, 2))}</pre>
      <div style="margin-top:8px;color:#8fbcbb;font-size:11px">
        In:  ${def.in.length ? def.in.join(', ') : '—'}<br>
        Out: ${def.out.length ? def.out.join(', ') : '—'}
      </div>
    </div>
  `;
}

function esc(s) {
  return String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// ─── Drag node + pan canvas ───────────────────────────────────────────
// Left click on a node body → drag the node (in canvas coordinates).
// Middle click or Space+left click on empty canvas → pan the view.
// Right click on canvas → context menu (handled below).
document.addEventListener('mousedown', (e) => {
  // Right click is for the context menu, not for dragging/panning.
  if (e.button === 2) return;

  const nodeEl = e.target.closest('.node');
  if (nodeEl && !e.target.classList.contains('port') && e.button === 0) {
    const id = nodeEl.dataset.id;
    const node = S.nodes.find(n => n.id === id);
    S.selected = id;
    const m = screenToCanvas(e.clientX, e.clientY);
    S.dragging = { id, dx: m.x - node.x, dy: m.y - node.y };
    nodeEl.classList.add('dragging');
    render();
    return;
  }

  // Pan: middle mouse, or left mouse while space is held, or on empty canvas.
  if (e.button === 1 || (e.button === 0 && S.spaceHeld)) {
    e.preventDefault();
    S.panning = { x: e.clientX, y: e.clientY, panX: S.panX, panY: S.panY };
    document.querySelector('.canvas-wrap').classList.add('panning');
  }
});

document.addEventListener('mousemove', (e) => {
  if (S.dragging) {
    const n = S.nodes.find(x => x.id === S.dragging.id);
    const m = screenToCanvas(e.clientX, e.clientY);
    n.x = m.x - S.dragging.dx;
    n.y = m.y - S.dragging.dy;
    render();
  }
  if (S.panning) {
    S.panX = S.panning.panX + (e.clientX - S.panning.x);
    S.panY = S.panning.panY + (e.clientY - S.panning.y);
    render();
  }
});

document.addEventListener('mouseup', () => {
  S.dragging = null;
  S.panning = null;
  document.querySelector('.canvas-wrap').classList.remove('panning');
  document.querySelectorAll('.node.dragging').forEach(n => n.classList.remove('dragging'));
});

// ─── Wheel zoom ───────────────────────────────────────────────────────
$('canvas-wrap').addEventListener('wheel', (e) => {
  e.preventDefault();
  const r = canvasWrapRect();
  const mx = e.clientX - r.left;
  const my = e.clientY - r.top;
  // Canvas point currently under the mouse.
  const cx = (mx - S.panX) / S.scale;
  const cy = (my - S.panY) / S.scale;
  const factor = e.deltaY < 0 ? 1.12 : 0.88;
  const newScale = Math.max(0.2, Math.min(3.0, S.scale * factor));
  // Adjust pan so the same canvas point stays under the mouse.
  S.panX = mx - cx * newScale;
  S.panY = my - cy * newScale;
  S.scale = newScale;
  render();
}, { passive: false });

// ─── Space key toggles pan mode cursor ─────────────────────────────────
document.addEventListener('keydown', (e) => {
  if (e.code === 'Space' && !S.spaceHeld) {
    S.spaceHeld = true;
    document.querySelector('.canvas-wrap').classList.add('space-pan');
  }
});
document.addEventListener('keyup', (e) => {
  if (e.code === 'Space') {
    S.spaceHeld = false;
    document.querySelector('.canvas-wrap').classList.remove('space-pan');
  }
});

// ─── Click port → start edge ─────────────────────────────────────────
document.addEventListener('mousedown', (e) => {
  if (e.button !== 0) return; // only left click starts a connection
  const port = e.target.closest('.port');
  if (!port) return;
  e.stopPropagation();
  S.connecting = {
    fromId: port.dataset.id,
    fromPort: port.dataset.port,
    fromDir: port.dataset.dir,
  };
});

document.addEventListener('mouseup', (e) => {
  if (!S.connecting) return;
  const port = e.target.closest('.port');
  if (!port) { S.connecting = null; return; }
  if (port.dataset.dir !== 'input') { S.connecting = null; return; } // output → input only
  const dup = S.edges.find(ed =>
    ed.source === S.connecting.fromId && ed.sourcePort === S.connecting.fromPort &&
    ed.target === port.dataset.id && ed.targetPort === port.dataset.port);
  if (!dup) {
    S.edges.push({
      id: `e-${Date.now()}-${Math.random().toString(36).slice(2,6)}`,
      source: S.connecting.fromId,
      sourcePort: S.connecting.fromPort,
      target: port.dataset.id,
      targetPort: port.dataset.port,
    });
    render();
  }
  S.connecting = null;
});

// ─── Reset / Run ─────────────────────────────────────────────────────
$('btn-reset').addEventListener('click', () => {
  loadSampleGraph();
  S.graphId = null;
  render();
  log('Layout reset (canvas only — backend graph unchanged).');
});

$('btn-run').addEventListener('click', async () => {
  if (S.running) return;
  S.running = true;
  $('btn-run').disabled = true;
  $('status').textContent = 'running…';
  try {
    await runForReal();
    $('status').textContent = 'done';
  } catch (e) {
    $('status').textContent = 'failed';
    log('!! ' + (e && e.message ? e.message : e));
  } finally {
    S.running = false;
    $('btn-run').disabled = false;
    for (const n of S.nodes) n.running = false;
    render();
  }
});

function log(msg) {
  const el = $('output-log');
  el.textContent += msg + '\n';
  el.scrollTop = el.scrollHeight;
}

// ─── Real backend integration ─────────────────────────────────────────
// The canvas state (S.nodes / S.edges) is mirrored to the backend:
//   1. ensure a graph row exists (S.graphId), replacing nodes+edges
//   2. POST /api/graphs/:gid/run  → run_id
//   3. poll /api/runs/:rid/output every 400 ms, render events live
// The server walks the graph; the UI only renders events.

const API = {
  async req(method, path, body) {
    const opt = { method, headers: {} };
    if (body !== undefined) {
      opt.headers['Content-Type'] = 'application/json';
      opt.body = JSON.stringify(body);
    }
    const r = await fetch(path, opt);
    const text = await r.text();
    let data = null;
    try { data = text ? JSON.parse(text) : null; } catch { data = { raw: text }; }
    if (!r.ok) throw new Error(`${method} ${path} → ${r.status} ${text}`);
    return data;
  },
  get: (p) => API.req('GET', p),
  post: (p, b) => API.req('POST', p, b),
  put: (p, b) => API.req('PUT', p, b),
  del: (p) => API.req('DELETE', p),
};

// Map frontend node → backend node shape (config passes through).
function toBackendNode(n) {
  return {
    type: n.type,
    label: n.label,
    config: n.config || {},
    position: { x: n.x, y: n.y },
  };
}

// Mirror the canvas to the backend. Returns a map frontendId → backendId.
async function syncGraphToBackend() {
  if (!S.graphId) {
    const g = await API.post('/api/graphs', {
      name: 'canvas-session',
      description: 'graph mirrored from the web canvas',
    });
    S.graphId = g.id;
    log(`created backend graph ${S.graphId.slice(0, 8)}…`);
  } else {
    // Wipe existing nodes (edges cascade) so the backend matches the canvas.
    const existing = await API.get(`/api/graphs/${S.graphId}`);
    for (const n of existing.nodes || []) {
      await API.del(`/api/graphs/${S.graphId}/nodes/${n.id}`);
    }
  }

  const idMap = {};
  for (const n of S.nodes) {
    const created = await API.post(`/api/graphs/${S.graphId}/nodes`, toBackendNode(n));
    idMap[n.id] = created.id;
  }
  for (const e of S.edges) {
    await API.post(`/api/graphs/${S.graphId}/edges`, {
      source: idMap[e.source],
      source_port: e.sourcePort,
      target: idMap[e.target],
      target_port: e.targetPort,
    });
  }
  return idMap;
}

// Collect run inputs from `input` nodes: param_name (or label) → value.
function collectRunInputs() {
  const inputs = {};
  for (const n of S.nodes) {
    if (n.type !== 'input') continue;
    const key = (n.config && (n.config.param_name || n.config.name)) || n.label;
    inputs[key] = (n.config && (n.config.value ?? n.config.default_value)) || '';
  }
  return inputs;
}

async function runForReal() {
  log('━━━ syncing graph to backend ━━━');
  const idMap = await syncGraphToBackend();
  const backToFront = {};
  for (const [f, b] of Object.entries(idMap)) backToFront[b] = f;

  log('━━━ POST /run ━━━');
  const run = await API.post(`/api/graphs/${S.graphId}/run`, { inputs: collectRunInputs() });
  const rid = run.run_id;
  log(`run_id ${rid.slice(0, 8)}…`);

  // Poll the event stream.
  let seen = 0;
  for (;;) {
    await new Promise(r => setTimeout(r, 400));
    const snap = await API.get(`/api/runs/${rid}/output`);
    const lines = (snap.output || '').split('\n').filter(x => x.trim());
    for (; seen < lines.length; seen++) {
      let ev;
      try { ev = JSON.parse(lines[seen]); } catch { continue; }
      renderEvent(ev, backToFront);
    }
    if (snap.finished) break;
  }
}

function renderEvent(ev, backToFront) {
  const fnode = ev.node_id ? S.nodes.find(n => n.id === backToFront[ev.node_id]) : null;
  switch (ev.event) {
    case 'graph_started':
      log('━━━ graph_started ━━━');
      break;
    case 'node_started':
      log(`  ▷ ${fnode ? fnode.type + ':' + fnode.label : ev.node_id}`);
      if (fnode) { fnode.running = true; render(); }
      break;
    case 'node_output': {
      const chunk = (ev.chunk || '').toString();
      log('      ' + chunk.replace(/\n/g, '\n      '));
      break;
    }
    case 'node_completed':
      if (fnode) { fnode.running = false; render(); }
      break;
    case 'node_failed':
    case 'node_error':
      log(`  !! ${ev.error || ev.message || 'node error'}`);
      break;
    case 'graph_completed':
      log('━━━ graph_completed ━━━');
      break;
    case 'graph_failed':
      log('━━━ graph_failed: ' + (ev.error || '?') + ' ━━━');
      break;
    case 'graph_cancelled':
      log('━━━ graph_cancelled ━━━');
      break;
    default:
      log('  · ' + JSON.stringify(ev));
  }
}

// ─── Node creation helpers ───────────────────────────────────────────
function uid() {
  return Math.random().toString(36).slice(2, 10) + Date.now().toString(36).slice(2, 6);
}

function createNode(type, x, y, label, config = {}) {
  const def = NODE_DEFS[type];
  const n = {
    id: `n-${uid()}`,
    type,
    label: label || (def.label + ' ' + (S.nodes.filter(n => n.type === type).length + 1)),
    x, y,
    config: { ...config },
  };
  S.nodes.push(n);
  return n;
}

function createLoopPair(x, y, loopCount = 3) {
  const start = createNode('loop_start', x, y, 'Loop Start', { loop_count: loopCount });
  const end   = createNode('loop_end',   x, y + LOOP_PAIR_SPACING, 'Loop End', {});
  S.edges.push({
    id: `e-${uid()}`,
    source: start.id, sourcePort: 'loop_count',
    target: end.id,   targetPort: 'loop_count',
  });
  S.selected = start.id;
  render();
  log(`Created Loop pair (count=${loopCount}). Wire body nodes between Loop Start and Loop End.`);
}

// ─── Context menu (right-click on canvas) ───────────────────────────
const menu = $('context-menu');
const subAdd = $('ctx-sub-add-node');

function showContextMenu(sx, sy, cx, cy) {
  menu.style.left = `${sx}px`;
  menu.style.top = `${sy}px`;
  menu.classList.remove('hidden');
  subAdd.classList.add('hidden');
  S.menuPos = { x: cx, y: cy };
}
function hideContextMenu() {
  menu.classList.add('hidden');
  subAdd.classList.add('hidden');
  S.menuPos = null;
}

$('canvas-wrap').addEventListener('contextmenu', (e) => {
  e.preventDefault();
  const m = screenToCanvas(e.clientX, e.clientY);
  showContextMenu(e.clientX, e.clientY, m.x, m.y);
});

// Toggle the Add node submenu on click.
$('ctx-add-node').addEventListener('click', (e) => {
  e.stopPropagation();
  subAdd.classList.toggle('hidden');
});

// Click a node type in the submenu to create a node there.
menu.addEventListener('click', (e) => {
  const nodeItem = e.target.closest('.ctx-node');
  if (nodeItem && S.menuPos) {
    const type = nodeItem.dataset.type;
    if (type === 'loop') createLoopPair(S.menuPos.x, S.menuPos.y, 3);
    else createNode(type, S.menuPos.x, S.menuPos.y);
    render();
    hideContextMenu();
  }
  if (e.target.id === 'ctx-reset-view') {
    resetView();
    render();
    hideContextMenu();
  }
  if (e.target.id === 'ctx-load-loop') {
    loadSampleGraph();
    S.graphId = null;
    resetView();
    render();
    log('Loaded loop demo graph.');
    hideContextMenu();
  }
  if (e.target.id === 'ctx-load-multi') {
    loadMultiAgentGraph();
    S.graphId = null;
    resetView();
    render();
    log('Loaded multi-agent demo graph.');
    hideContextMenu();
  }
});

// Hide menu when clicking elsewhere.
document.addEventListener('click', (e) => {
  if (!e.target.closest('.context-menu')) hideContextMenu();
});

// ─── Help overlay ────────────────────────────────────────────────────
$('btn-help').addEventListener('click', () => $('help-overlay').classList.remove('hidden'));
$('btn-help-close').addEventListener('click', () => $('help-overlay').classList.add('hidden'));
$('help-overlay').addEventListener('click', (e) => {
  if (e.target === $('help-overlay')) $('help-overlay').classList.add('hidden');
});

// ─── Reset / Run ─────────────────────────────────────────────────────
$('btn-reset').addEventListener('click', () => {
  loadSampleGraph();
  S.graphId = null;
  resetView();
  render();
  log('Layout reset (canvas only — backend graph unchanged).');
});

// ─── Init ────────────────────────────────────────────────────────────
loadSampleGraph();
render();
log('Loaded sample loop demo. Right-click the canvas to add nodes, scroll to zoom, middle-drag to pan.');
