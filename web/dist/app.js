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

// ─── Inspector field schemas ─────────────────────────────────────────────
// Per-node-type form definitions for the Inspector panel. Each field:
//   key         config property name
//   label       human-readable label
//   kind        'text' | 'textarea' | 'number' | 'select'
//   placeholder / help   UI hints
//   options     [{value,label}] for 'select'
//   min / max   for 'number'
// The agent node is intentionally schema-less for now (raw JSON editor).
const FIELD_SCHEMAS = {
  input: [
    { key: 'param_name', label: 'Parameter name', kind: 'text',
      placeholder: 'topic',
      help: 'Key looked up in the Run payload (POST body "inputs").' },
    { key: 'value', label: 'Default value', kind: 'textarea', rows: 3,
      placeholder: 'Used when the Run payload has no value for this key',
      help: 'Fallback text when the run provides nothing.' },
  ],
  prompt: [
    { key: 'value', label: 'Prompt text', kind: 'textarea', rows: 6,
      placeholder: 'Write the static text this node emits…',
      help: 'Emitted verbatim on the "out" port.' },
  ],
  output: [
    { key: 'output_name', label: 'Result name', kind: 'text',
      placeholder: 'verdict',
      help: 'Key under which this value appears in the run results.' },
  ],
  merge: [
    { key: 'strategy', label: 'Join strategy', kind: 'select',
      options: [{ value: 'concat', label: 'Concatenate' }],
      help: 'How the inputs are combined into one text.' },
    { key: 'separator', label: 'Separator', kind: 'text',
      placeholder: '\\n\\n',
      help: 'Inserted between inputs (concat strategy).' },
  ],
  loop_start: [
    { key: 'loop_count', label: 'Iterations', kind: 'number', min: 1, max: 100,
      help: 'How many times the loop body runs.' },
  ],
  loop_end: [
    { key: 'loop_count', label: 'Target iterations', kind: 'display',
      value: (n) => {
        const st = findLoopStartFor(n.id);
        return st ? String(st.config.loop_count ?? 1) : '—';
      },
      help: 'Set on the paired Loop Start node (display only).' },
  ],

};

// ─── Agent metadata cache ─────────────────────────────────────────────────
// Async-populated data for the Agent inspector: models, tools, skills, MCP
// servers. Fetched lazily on first inspector open, cached afterwards.
// Call AgentMeta.refresh() to force a refetch (e.g. after settings change).
const AgentMeta = (() => {
  const cache = {};   // key -> {status:'loading'|'ok'|'error', data, error}
  const listeners = new Set();

  function notify() { [...listeners].forEach(fn => fn()); }

  async function fetchKey(key, url) {
    if (cache[key] && cache[key].status !== 'error') return cache[key];
    cache[key] = { status: 'loading', data: null };
    try {
      const r = await fetch(url);
      if (!r.ok) throw new Error('HTTP ' + r.status);
      cache[key] = { status: 'ok', data: await r.json() };
    } catch (e) {
      cache[key] = { status: 'error', data: null, error: String(e) };
    }
    notify();
    return cache[key];
  }

  return {
    models: () => fetchKey('models', '/api/models'),
    tools:  () => fetchKey('tools',  '/api/agent/tools'),
    skills: () => fetchKey('skills', '/api/agent/skills'),
    mcp:    () => fetchKey('mcp',    '/api/agent/mcp'),
    get: (key) => cache[key],
    onChange: (fn) => listeners.add(fn),
    refresh: () => { for (const k of Object.keys(cache)) delete cache[k]; },
  };
})();

const LOOP_PAIR_SPACING = 220; // px between loop_start and loop_end

const NODE_WIDTH = 220;

const S = {
  nodes: [],
  edges: [],
  selected: null,
  dragging: null,
  connecting: null,
  selectedEdge: null,
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
    mk('agent',  'sqlite-pro',  280, 60,  { model: 'MiniMax-M2.7', project: '~/work/sqlite' }),
    mk('agent',  'postgres-pro',280, 220, { model: 'MiniMax-M2.7', project: '~/work/postgres' }),
    mk('merge',  'merge',       520, 140, { strategy: 'concat' }),
    mk('agent',  'synthesiser', 760, 140, { model: 'MiniMax-M2.7' }),
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

// Full render: structure (nodes) first, then wires (which read live port
// positions from the fresh DOM), then the inspector. For cheap updates
// call the individual renderers — see the drag/pan handlers below.
function render() {
  applyTransform();
  renderNodes();
  renderEdges();
  renderInspector();
}

function applyTransform() {
  const transform = `translate(${S.panX.toFixed(1)}px, ${S.panY.toFixed(1)}px) scale(${S.scale.toFixed(4)})`;
  $('nodes').style.transform = transform;
  $('nodes').style.transformOrigin = '0 0';
  $('edges').style.transform = transform;
  $('edges').style.transformOrigin = '0 0';
}

function renderEdges() {
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
    const dAttr = `M ${srcPt.x.toFixed(1)} ${srcPt.y.toFixed(1)} C ${cp1x.toFixed(1)} ${srcPt.y.toFixed(1)}, ${cp2x.toFixed(1)} ${dstPt.y.toFixed(1)}, ${dstPt.x.toFixed(1)} ${dstPt.y.toFixed(1)}`;
    // Fat invisible hit area — a 2px bezier is nearly unclickable.
    const hit = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    hit.setAttribute('d', dAttr);
    hit.setAttribute('class', 'edge-hit');
    hit.dataset.eid = edge.id;
    svg.appendChild(hit);

    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', dAttr);
    let cls = 'edge-wire';
    if (src.running || dst.running) cls += ' active';
    if (S.selectedEdge === edge.id) cls += ' selected';
    path.setAttribute('class', cls);
    path.dataset.eid = edge.id;
    path.setAttribute('title', `${esc(edge.sourcePort)} → ${esc(edge.targetPort)} — click to select, Delete to break`);
    svg.appendChild(path);
  }
  // The svg was just wiped — re-add the in-progress wire if the user is
  // mid-drag (e.g. a node drag triggered renderEdges() during wiring).
  if (S.connecting) drawPendingEdge();
}

function renderNodes() {
  // Mark ports that participate in an edge so we can style them as connected.
  const connectedPorts = new Set();
  for (const e of S.edges) {
    connectedPorts.add(`${e.source}:${e.sourcePort}:output`);
    connectedPorts.add(`${e.target}:${e.targetPort}:input`);
  }

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
  if (n.type === 'loop_end')    {
    const st = findLoopStartFor(n.id);
    return { text: st ? `loops: ${st.config.loop_count ?? 1}` : 'loop end', muted: !st };
  }
  return { text: '', muted: true };
}

function renderInspector() {
  const i = $('inspector-body');
  const n = S.nodes.find(x => x.id === S.selected);
  if (!n) { i.innerHTML = '<p class="muted">Click a node to see its config.</p>'; return; }
  const def = NODE_DEFS[n.type];
  const schema = FIELD_SCHEMAS[n.type];

  let fieldsHtml = '';
  if (schema) {
    fieldsHtml = schema.map(f => inspFieldHtml(n, f)).join('');
  } else if (n.type === 'agent') {
    fieldsHtml = renderAgentFields(n);
  }

  i.innerHTML = `
    <div class="node-card">
      <div class="insp-field">
        <label class="insp-label" for="insp-label">Name</label>
        <input id="insp-label" class="insp-input" type="text"
               value="${esc(n.label)}" data-insplabel="1">
      </div>
      <div class="node-card-type">${def.label} · ${n.type} · id ${esc(n.id)}</div>
      ${fieldsHtml}
      <div class="insp-ports">
        In:  ${def.in.length ? def.in.join(', ') : '—'}<br>
        Out: ${def.out.length ? def.out.join(', ') : '—'}
      </div>
      <details class="insp-raw">
        <summary>Raw JSON</summary>
        <pre>${esc(JSON.stringify(n.config, null, 2))}</pre>
      </details>
    </div>
  `;

  // Bind label editor
  i.querySelector('[data-insplabel]').addEventListener('input', (e) => {
    n.label = e.target.value;
    refreshNodeChrome(n);     // canvas title updates live; inspector keeps focus
  });

  // Bind agent fields (async-populated). When the metadata fetches land,
  // a single global AgentMeta.onChange listener (registered at init)
  // re-renders the inspector if an agent node is still selected.
  if (n.type === 'agent') {
    bindAgentFields(n, i);
  }

  // Bind schema fields
  if (schema) {
    for (const f of schema) {
      if (f.kind === 'display') continue;
      const el = i.querySelector(`[data-inspkey="${f.key}"]`);
      if (!el) continue;
      el.addEventListener('input', (e) => {
        n.config[f.key] = f.kind === 'number' ? Number(e.target.value) : e.target.value;
        refreshNodeChrome(n); // node sub-label updates live
        // loop_start owns the iteration setting — mirror it onto the
        // paired loop_end so backend payloads stay consistent.
        if (n.type === 'loop_start' && f.key === 'loop_count') {
          const end = S.nodes.find(x =>
            S.edges.some(e => e.source === n.id && e.sourcePort === 'loop_count'
                           && e.target === x.id && e.targetPort === 'loop_count'));
          if (end) { end.config.loop_count = n.config.loop_count; refreshNodeChrome(end); }
        }
      });
    }
  }
}

// Find the loop_start paired with a loop_end via the permanent
// start:loop_count → end:loop_count edge.
function findLoopStartFor(endId) {
  const e = S.edges.find(e => e.target === endId && e.targetPort === 'loop_count');
  return e ? S.nodes.find(n => n.id === e.source) : null;
}

// ─── Agent inspector fields ───────────────────────────────────────────────
// Renders the Agent node's editor: flat fields (model, project, system
// prompt, auto-approve) plus three collapsible checkbox groups (tools,
// skills, MCP servers). Unchecked items are stored as *_disabled arrays in
// the node config — daedalus' AgentConfig uses disable-lists, so "everything
// on by default" is the empty array.

function agentCfg(n, key, def) {
  return n.config[key] !== undefined ? n.config[key] : def;
}

function renderAgentFields(n) {
  const modelsSt = AgentMeta.get('models');
  const toolsSt  = AgentMeta.get('tools');
  const skillsSt = AgentMeta.get('skills');
  const mcpSt    = AgentMeta.get('mcp');

  // Kick off fetches (idempotent while cached).
  AgentMeta.models(); AgentMeta.tools(); AgentMeta.skills(); AgentMeta.mcp();

  // ── Model select, grouped by provider ──
  let modelOpts = '';
  const cur = agentCfg(n, 'model', '');
  if (modelsSt && modelsSt.status === 'ok') {
    const byProvider = {};
    for (const m of modelsSt.data) {
      (byProvider[m.provider || 'Other'] ??= []).push(m);
    }
    for (const [prov, list] of Object.entries(byProvider)) {
      modelOpts += `<optgroup label="${esc(prov)}">` + list.map(m =>
        `<option value="${esc(m.name)}"${m.name === cur ? ' selected' : ''}${m.has_key ? '' : ' disabled'}>
           ${esc(m.name)}${m.has_key ? '' : ' (no key)'}${m.is_default ? ' · default' : ''}
         </option>`).join('') + '</optgroup>';
    }
    // Current value not in the list (renamed / removed model) — keep it.
    if (cur && !modelsSt.data.some(m => m.name === cur)) {
      modelOpts = `<option value="${esc(cur)}" selected>${esc(cur)} (unlisted)</option>` + modelOpts;
    }
  } else {
    modelOpts = `<option value="${esc(cur)}" selected>${esc(cur || (modelsSt && modelsSt.status === 'error' ? '(failed to load models)' : 'loading…'))}</option>`;
  }

  const autoApprove = agentCfg(n, 'auto_approve', true);
  const mcpEnabled  = agentCfg(n, 'mcp_enabled', true);

  return `
    <div class="insp-field">
      <label class="insp-label" for="insp-agent-model">Model</label>
      <select id="insp-agent-model" class="insp-input" data-agent-key="model">${modelOpts}</select>
      <div class="insp-help">Provider/model used for this agent's LLM calls.</div>
    </div>
    <div class="insp-field">
      <label class="insp-label">Project root</label>
      <input class="insp-input" type="text" data-agent-key="project"
             value="${esc(agentCfg(n, 'project', ''))}" placeholder="/path/to/project">
      <div class="insp-help">Working directory for the agent's tools.</div>
    </div>
    <div class="insp-field">
      <label class="insp-label">System prompt</label>
      <textarea class="insp-input insp-textarea" rows="4" data-agent-key="system_prompt"
        placeholder="(optional — overrides the composed default)">${esc(agentCfg(n, 'system_prompt', ''))}</textarea>
      <div class="insp-help">Persona / role override, prepended to the system prompt.</div>
    </div>
    <div class="insp-field insp-check">
      <label><input type="checkbox" data-agent-key="auto_approve"${autoApprove ? ' checked' : ''}>
        Auto-approve shell commands</label>
      <div class="insp-help">Run execute_bash without asking (recommended in graphs).</div>
    </div>
    <div class="insp-field insp-check">
      <label><input type="checkbox" data-agent-key="mcp_enabled"${mcpEnabled ? ' checked' : ''}>
        Enable MCP servers</label>
      <div class="insp-help">Master switch for all MCP integrations.</div>
    </div>
    ${agentGroupHtml(n, 'tools',  'Tools',       toolsSt,  'tools_disabled')}
    ${agentGroupHtml(n, 'skills', 'Skills',      skillsSt, 'skills_disabled')}
    ${agentGroupHtml(n, 'mcp',    'MCP servers', mcpSt,    'mcp_servers_disabled')}
  `;
}

// Collapsible checkbox group for one of tools / skills / mcp.
function agentGroupHtml(n, key, title, st, disabledKey) {
  const disabled = new Set(agentCfg(n, disabledKey, []));
  let body, summary;
  if (!st || st.status === 'loading') {
    summary = 'loading…';
    body = '<div class="insp-help">Loading…</div>';
  } else if (st.status === 'error') {
    summary = 'failed to load';
    body = `<div class="insp-help">Error: ${esc(st.error || 'unknown')}</div>`;
  } else {
    const items = st.data.map(x => typeof x === 'string' ? x : x.name);
    if (items.length === 0) {
      summary = 'none configured';
      body = '<div class="insp-help">None found on the server.</div>';
    } else {
      const enabled = items.filter(x => !disabled.has(x)).length;
      summary = `${enabled} of ${items.length} enabled`;
      body = items.map(name =>
        `<label class="insp-check-item">
           <input type="checkbox" data-agent-group="${disabledKey}" value="${esc(name)}"
             ${disabled.has(name) ? '' : ' checked'}> ${esc(name)}
         </label>`).join('');
    }
  }
  return `<details class="insp-group" data-group="${key}">
    <summary>${esc(title)} <span class="insp-group-count">${esc(summary)}</span></summary>
    <div class="insp-group-body">${body}</div>
  </details>`;
}

// Bind agent fields after renderInspector injects the HTML.
function bindAgentFields(n, container) {
  container.querySelectorAll('[data-agent-key]').forEach(el => {
    const key = el.dataset.agentKey;
    el.addEventListener(el.type === 'checkbox' ? 'change' : 'input', (e) => {
      n.config[key] = el.type === 'checkbox' ? el.checked : e.target.value;
      refreshNodeChrome(n);
    });
  });
  container.querySelectorAll('[data-agent-group]').forEach(el => {
    el.addEventListener('change', () => {
      const key = el.dataset.agentGroup;
      const set = new Set(agentCfg(n, key, []));
      if (el.checked) set.delete(el.value); else set.add(el.value);
      n.config[key] = [...set];
      // Update the "N of M enabled" summary without a full re-render.
      const group = el.closest('.insp-group');
      const boxes = group.querySelectorAll('[data-agent-group]');
      const on = [...boxes].filter(b => b.checked).length;
      group.querySelector('.insp-group-count').textContent = `${on} of ${boxes.length} enabled`;
    });
  });
}

function inspFieldHtml(n, f) {
  if (f.kind === 'display') {
    const v = typeof f.value === 'function' ? f.value(n) : (n.config[f.key] ?? '—');
    return `<div class="insp-field">
    <label class="insp-label">${esc(f.label)}</label>
    <div class="insp-display">${esc(v)}</div>
    ${f.help ? `<div class="insp-help">${esc(f.help)}</div>` : ''}
  </div>`;
  }
  const v = n.config[f.key] ?? '';
  const ph = f.placeholder ? `placeholder="${esc(f.placeholder)}"` : '';
  const help = f.help ? `<div class="insp-help">${esc(f.help)}</div>` : '';
  let control;
  if (f.kind === 'textarea') {
    control = `<textarea class="insp-input insp-textarea" rows="${f.rows || 4}"
      data-inspkey="${f.key}" ${ph}>${esc(v)}</textarea>`;
  } else if (f.kind === 'number') {
    control = `<input class="insp-input" type="number" data-inspkey="${f.key}"
      value="${esc(v)}" min="${f.min ?? ''}" max="${f.max ?? ''}" ${ph}>`;
  } else if (f.kind === 'select') {
    const opts = (f.options || []).map(o =>
      `<option value="${esc(o.value)}"${o.value === v ? ' selected' : ''}>${esc(o.label)}</option>`).join('');
    control = `<select class="insp-input" data-inspkey="${f.key}">${opts}</select>`;
  } else {
    control = `<input class="insp-input" type="text" data-inspkey="${f.key}"
      value="${esc(v)}" ${ph}>`;
  }
  return `<div class="insp-field">
    <label class="insp-label">${esc(f.label)}</label>
    ${control}
    ${help}
  </div>`;
}

// Update one node's canvas chrome (title + sub-label) in place, without a
// full render(). Used by the Inspector so typing doesn't rebuild the
// inspector itself (which would steal focus mid-keystroke).
function refreshNodeChrome(n) {
  const el = document.querySelector(`.node[data-id="${n.id}"]`);
  if (!el) return;
  const def = NODE_DEFS[n.type];
  const title = el.querySelector('.node-title');
  if (title) {
    title.innerHTML = `
          <span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:${def.color}"></span>
          ${esc(n.label)}
          <span class="node-type">${n.type}</span>`;
  }
  const sub = nodeSub(n);
  const subEl = el.querySelector('.node-sub');
  if (subEl) {
    subEl.textContent = sub.text;
    subEl.classList.toggle('muted', !!sub.muted);
  }
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
    // Update selection highlight in place — a full render here would
    // rebuild every node div (innerHTML='') and destroy the very element
    // being grabbed, dropping the drag gesture.
    document.querySelectorAll('.node.selected').forEach(el => el.classList.remove('selected'));
    nodeEl.classList.add('selected');
    nodeEl.classList.add('dragging');
    renderInspector();
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
    // Move just this node's div (no DOM rebuild → no flicker) and redraw
    // the wires — portCenter() reads the live rect, so wires track exactly.
    const el = document.querySelector(`.node[data-id="${CSS.escape(n.id)}"]`);
    if (el) { el.style.left = n.x + 'px'; el.style.top = n.y + 'px'; }
    renderEdges();
  }
  if (S.panning) {
    S.panX = S.panning.panX + (e.clientX - S.panning.x);
    S.panY = S.panning.panY + (e.clientY - S.panning.y);
    // Both layers share the transform — nothing needs redrawing.
    applyTransform();
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
  applyTransform();
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

// ─── Click-and-drag wiring ────────────────────────────────────────────
// Press on any port, drag (a dashed preview wire follows the cursor and
// valid drop-target ports light up), release on a matching port to create
// the edge. Works in both directions — out→in and in→out both produce an
// output→input edge. Release anywhere else (or press ESC) to cancel.

function startConnecting(port) {
  S.connecting = {
    fromId:   port.dataset.id,
    fromPort: port.dataset.port,
    fromDir:  port.dataset.dir,
    cursor:   null,   // canvas coords of the last cursor position
  };
  document.querySelector('.canvas-wrap').classList.add('wiring');
  markDropTargets();
}

function markDropTargets() {
  const wantDir = S.connecting.fromDir === 'output' ? 'input' : 'output';
  document.querySelectorAll(`.port[data-dir="${wantDir}"]`).forEach(el => {
    if (el.dataset.id !== S.connecting.fromId) el.classList.add('drop-target');
  });
}

function clearWiringUi() {
  document.querySelector('.canvas-wrap').classList.remove('wiring');
  document.querySelectorAll('.port.drop-target').forEach(el => el.classList.remove('drop-target'));
  document.getElementById('pending-edge')?.remove();
}

function drawPendingEdge() {
  if (!S.connecting || !S.connecting.cursor) return;
  const srcNode = S.nodes.find(n => n.id === S.connecting.fromId);
  if (!srcNode) return;
  const srcPt = portCenter(srcNode, S.connecting.fromPort, S.connecting.fromDir);
  const dstPt = S.connecting.cursor;
  // Curve in the direction of travel: out-drags bend right, in-drags left.
  const sign = S.connecting.fromDir === 'output' ? 1 : -1;
  const dx = dstPt.x - srcPt.x;
  const cp = Math.max(40, Math.abs(dx) * 0.45);
  const d = `M ${srcPt.x.toFixed(1)} ${srcPt.y.toFixed(1)} C ${(srcPt.x + sign * cp).toFixed(1)} ${srcPt.y.toFixed(1)}, ${(dstPt.x - sign * cp).toFixed(1)} ${dstPt.y.toFixed(1)}, ${dstPt.x.toFixed(1)} ${dstPt.y.toFixed(1)}`;
  let path = document.getElementById('pending-edge');
  if (!path) {
    path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('id', 'pending-edge');
    path.setAttribute('class', 'pending');
    $('edges').appendChild(path);
  }
  path.setAttribute('d', d);
}

document.addEventListener('mousedown', (e) => {
  if (e.button !== 0) return; // only left click starts a connection
  const port = e.target instanceof Element ? e.target.closest('.port') : null;
  if (!port) return;
  e.stopPropagation();
  startConnecting(port);
});

document.addEventListener('mousemove', (e) => {
  if (!S.connecting) return;
  S.connecting.cursor = screenToCanvas(e.clientX, e.clientY);
  drawPendingEdge();
});

document.addEventListener('mouseup', (e) => {
  if (!S.connecting) return;
  const from = S.connecting;
  S.connecting = null;
  clearWiringUi();

  // e.target can be document/window in edge cases (e.g. mouse released
  // outside the window) — guard before calling closest().
  const port = e.target instanceof Element ? e.target.closest('.port') : null;
  if (!port) return;                          // released on empty canvas → cancel
  if (port.dataset.id === from.fromId) return; // no self-connections

  // Normalise to output → input regardless of drag direction.
  let source, sourcePort, target, targetPort;
  if (from.fromDir === 'output' && port.dataset.dir === 'input') {
    source = from.fromId;       sourcePort = from.fromPort;
    target = port.dataset.id;   targetPort = port.dataset.port;
  } else if (from.fromDir === 'input' && port.dataset.dir === 'output') {
    source = port.dataset.id;   sourcePort = port.dataset.port;
    target = from.fromId;       targetPort = from.fromPort;
  } else {
    return;                     // same-direction: not a valid connection
  }

  const dup = S.edges.find(ed =>
    ed.source === source && ed.sourcePort === sourcePort &&
    ed.target === target && ed.targetPort === targetPort);
  if (dup) return;

  S.edges.push({
    id: `e-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`,
    source, sourcePort, target, targetPort,
  });
  render();
});

// Click a wire to select it; click anything else to deselect.
document.addEventListener('click', (e) => {
  const wire = e.target instanceof Element ? e.target.closest('#edges path') : null;
  if (wire && wire.dataset.eid) {
    S.selectedEdge = wire.dataset.eid;
    renderEdges();
    return;
  }
  if (S.selectedEdge) {
    S.selectedEdge = null;
    renderEdges();
  }
});

// Delete / Backspace breaks the selected connection. Never while the
// user is typing in a form field (inspector, workflow name, …).
document.addEventListener('keydown', (e) => {
  if (e.code !== 'Delete' && e.code !== 'Backspace') return;
  if (!S.selectedEdge) return;
  if (e.target instanceof HTMLElement &&
      (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.isContentEditable)) return;
  deleteEdge(S.selectedEdge);
});

// Delete / Backspace also deletes the selected NODE (when no wire is
// selected) — the node's wires go with it. Never while typing in a field.
document.addEventListener('keydown', (e) => {
  if (e.code !== 'Delete' && e.code !== 'Backspace') return;
  if (e.target instanceof HTMLElement &&
      (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.isContentEditable)) return;
  if (S.selectedEdge) return;               // wire deletion handled above
  if (!S.selected) return;
  e.preventDefault();
  deleteNode(S.selected);
});

// ESC also deselects a wire (in addition to cancelling an in-progress wire).
document.addEventListener('keydown', (e) => {
  if (e.code === 'Escape' && S.selectedEdge) {
    S.selectedEdge = null;
    renderEdges();
  }
});

document.addEventListener('keydown', (e) => {
  if (e.code === 'Escape' && S.connecting) {
    S.connecting = null;
    clearWiringUi();
  }
});

// ─── Workflow save / export / import / switch ─────────────────────────
async function refreshWorkflowList() {
  try {
    const graphs = await API.get('/api/graphs');
    const sel = $('wf-switcher');
    sel.innerHTML = '<option value="">workflow…</option>' +
      graphs.map(g => `<option value="${g.id}"${g.id === S.graphId ? ' selected' : ''}>${esc(g.name)}</option>`).join('');
  } catch (e) { /* server offline — switcher stays empty */ }
}

function loadBackendGraph(g) {
  S.graphId = g.id;
  S.nodes = (g.nodes || []).map(n => ({
    id: n.id, type: n.type, label: n.label,
    x: n.x ?? n.position_x ?? (n.position && n.position.x) ?? 0,
    y: n.y ?? n.position_y ?? (n.position && n.position.y) ?? 0,
    config: n.config ?? n.config_json ?? {},
  }));
  S.edges = (g.edges || []).map(e => ({
    id: e.id || `${e.source}-${e.source_port}-${e.target}-${e.target_port}`,
    source: e.source ?? e.source_node, sourcePort: e.source_port,
    target: e.target ?? e.target_node, targetPort: e.target_port,
  }));
  S.selected = null;
  render();
}

$('wf-switcher').addEventListener('change', async (e) => {
  if (!e.target.value) return;
  const g = await API.get(`/api/graphs/${e.target.value}`);
  loadBackendGraph(g);
  log(`opened workflow "${g.meta ? g.meta.name : g.name}"`);
});

$('btn-save').addEventListener('click', async () => {
  await syncGraphToBackend();
  await refreshWorkflowList();
  log('workflow saved to server');
});

$('btn-export').addEventListener('click', async () => {
  await syncGraphToBackend();
  const resp = await fetch(`/api/graphs/${S.graphId}/export`);
  const blob = await resp.blob();
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'workflow.hivemind.json';
  a.click();
  URL.revokeObjectURL(a.href);
  log('exported workflow.hivemind.json');
});

$('btn-import').addEventListener('click', () => $('wf-file').click());
$('wf-file').addEventListener('change', async (e) => {
  const f = e.target.files[0];
  if (!f) return;
  const text = await f.text();
  const r = await API.post('/api/graphs/import', JSON.parse(text));
  const g = await API.get(`/api/graphs/${r.graph_id}`);
  loadBackendGraph(g);
  await refreshWorkflowList();
  log(`imported "${r.name}" (${r.nodes} nodes)`);
  e.target.value = '';
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

// ─── Auto layout (layered DAG layout, "Sugiyama-lite") ──────────────
// Longest-path layering over the edge DAG: every node gets a column =
// longest distance from a source; columns stack their nodes vertically
// centered against the tallest column. Loop feedback edges are NOT
// stored (the runner injects feedback), so real graphs are acyclic —
// but we defensively park any unlayered leftovers (cycles) in the last
// column instead of looping forever.
const LAYOUT_H_SPACING = 300;   // column pitch (node is 220px wide)
const LAYOUT_V_SPACING = 170;   // row pitch inside a column
const LAYOUT_MARGIN    = 60;

function autoLayout() {
  if (S.nodes.length === 0) return;

  const layer = new Map();   // node id -> column
  const indeg = new Map();
  const adj   = new Map();
  for (const n of S.nodes) { indeg.set(n.id, 0); adj.set(n.id, []); }
  for (const e of S.edges) {
    if (!indeg.has(e.source) || !indeg.has(e.target)) continue;
    adj.get(e.source).push(e.target);
    indeg.set(e.target, indeg.get(e.target) + 1);
  }

  // Kahn's algorithm with longest-path layering. Once a node's indegree
  // hits 0 all predecessors are placed, so its layer is final.
  let queue = S.nodes.filter(n => indeg.get(n.id) === 0).map(n => n.id);
  for (const id of queue) layer.set(id, 0);
  let guard = S.nodes.length * 4 + 16;
  while (queue.length && guard-- > 0) {
    const next = [];
    for (const id of queue) {
      for (const t of adj.get(id)) {
        const want = layer.get(id) + 1;
        if (!layer.has(t) || layer.get(t) < want) layer.set(t, want);
        indeg.set(t, indeg.get(t) - 1);
        if (indeg.get(t) === 0) next.push(t);
      }
    }
    queue = next;
  }
  // Defensive: nodes stuck in a cycle get parked after the real columns.
  let maxLayer = -1;
  for (const v of layer.values()) maxLayer = Math.max(maxLayer, v);
  for (const n of S.nodes) if (!layer.has(n.id)) layer.set(n.id, maxLayer + 1);

  // Group by column, keep each column's current top-to-bottom order
  // (stable, so the user's relative arrangement survives where possible).
  const cols = new Map();
  for (const n of S.nodes) {
    const c = layer.get(n.id);
    if (!cols.has(c)) cols.set(c, []);
    cols.get(c).push(n);
  }
  const colKeys = [...cols.keys()].sort((a, b) => a - b);
  for (const c of colKeys) cols.get(c).sort((a, b) => (a.y - b.y) || (a.x - b.x));

  const maxRows = Math.max(...colKeys.map(c => cols.get(c).length));
  colKeys.forEach((c, i) => {   // i (not c): compresses empty columns
    const nodes = cols.get(c);
    const y0 = LAYOUT_MARGIN + (maxRows - nodes.length) * LAYOUT_V_SPACING / 2;
    nodes.forEach((n, j) => {
      n.x = LAYOUT_MARGIN + i * LAYOUT_H_SPACING;
      n.y = y0 + j * LAYOUT_V_SPACING;
    });
  });
}

// Fit the current node bounding box into the viewport (used after layout).
function fitView() {
  if (!S.nodes.length) return;
  const wrap = $('canvas-wrap').getBoundingClientRect();
  const pad = 80;
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const n of S.nodes) {
    minX = Math.min(minX, n.x);           minY = Math.min(minY, n.y);
    maxX = Math.max(maxX, n.x + 220);     maxY = Math.max(maxY, n.y + 160);
  }
  const w = maxX - minX + pad * 2, h = maxY - minY + pad * 2;
  S.scale = Math.max(0.2, Math.min(wrap.width / w, wrap.height / h, 1.5));
  S.panX = wrap.width  / 2 - ((minX + maxX) / 2) * S.scale;
  S.panY = wrap.height / 2 - ((minY + maxY) / 2) * S.scale;
}

// ─── Deletion helpers ───────────────────────────────────────────────
// Delete a node and every wire attached to it.
function deleteNode(nodeId) {
  const n = S.nodes.find(x => x.id === nodeId);
  S.edges = S.edges.filter(e => e.source !== nodeId && e.target !== nodeId);
  S.nodes = S.nodes.filter(x => x.id !== nodeId);
  if (S.selected === nodeId) S.selected = null;
  if (S.selectedEdge && !S.edges.find(e => e.id === S.selectedEdge)) S.selectedEdge = null;
  render();
  log(`Deleted node "${n ? n.label : nodeId}" and its connections.`);
}

// Delete a single wire.
function deleteEdge(edgeId) {
  S.edges = S.edges.filter(e => e.id !== edgeId);
  if (S.selectedEdge === edgeId) S.selectedEdge = null;
  render();
  log('Connection removed.');
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
  // Show target-specific items only when the right-click landed on one.
  $('ctx-delete-node').classList.toggle('hidden', S.menuTarget?.kind !== 'node');
  $('ctx-delete-edge').classList.toggle('hidden', S.menuTarget?.kind !== 'edge');
}
function hideContextMenu() {
  menu.classList.add('hidden');
  subAdd.classList.add('hidden');
  S.menuPos = null;
  S.menuTarget = null;
}

$('canvas-wrap').addEventListener('contextmenu', (e) => {
  e.preventDefault();
  const m = screenToCanvas(e.clientX, e.clientY);

  // What is under the cursor? node > wire > bare canvas.
  S.menuTarget = null;
  const nodeEl = e.target.closest('.node');
  const wireEl = e.target.closest('path[data-eid]');
  if (nodeEl) {
    S.menuTarget = { kind: 'node', id: nodeEl.dataset.id };
    // Right-click also selects the node (inspector + highlight follow).
    S.selected = S.menuTarget.id;
    render();
  } else if (wireEl) {
    S.menuTarget = { kind: 'edge', id: wireEl.dataset.eid };
    S.selectedEdge = S.menuTarget.id;
    render();
  }
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
  if (e.target.id === 'ctx-delete-node' && S.menuTarget?.kind === 'node') {
    deleteNode(S.menuTarget.id);
    hideContextMenu();
  }
  if (e.target.id === 'ctx-delete-edge' && S.menuTarget?.kind === 'edge') {
    deleteEdge(S.menuTarget.id);
    hideContextMenu();
  }
  if (e.target.id === 'ctx-reset-view') {
    resetView();
    render();
    hideContextMenu();
  }
  if (e.target.id === 'ctx-auto-layout') {
    autoLayout();
    fitView();
    render();
    log('Auto layout applied.');
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

// ─── Panel layout (Phase 2): splitters, collapse, persistence ─────────
const Layout = (() => {
  const KEY = 'hm.layout';
  const app = document.querySelector('.app');
  const DEF = { inspW: 286, outH: 166, inspCollapsed: false, outCollapsed: false };
  let st = { ...DEF };

  function load() {
    try {
      const raw = localStorage.getItem(KEY);
      if (raw) Object.assign(st, JSON.parse(raw));
    } catch { /* corrupt storage → defaults */ }
  }
  function save() {
    try { localStorage.setItem(KEY, JSON.stringify(st)); } catch { /* ignore */ }
  }
  function apply() {
    app.classList.toggle('insp-collapsed', st.inspCollapsed);
    app.classList.toggle('out-collapsed', st.outCollapsed);
    app.style.setProperty('--insp-w', st.inspW + 'px');
    app.style.setProperty('--out-h', st.outH + 'px');
    const bi = $('btn-collapse-insp'), bo = $('btn-collapse-out');
    bi.textContent = st.inspCollapsed ? '\u27e9' : '\u27e8';
    bi.title = st.inspCollapsed ? 'Expand inspector' : 'Collapse inspector';
    bo.textContent = st.outCollapsed ? '\u25b4' : '\u25be';
    bo.title = st.outCollapsed ? 'Expand output' : 'Collapse output';
  }

  const clampInsp = (w) => Math.max(200, Math.min(w, window.innerWidth * 0.5));
  const clampOut  = (h) => Math.max(60, Math.min(h, window.innerHeight * 0.7));

  function wireSplitter(el, onDelta) {
    const isV = el.classList.contains('splitter-v');
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      el.classList.add('dragging');
      document.body.classList.add('splitter-dragging');
      let last = isV ? e.clientX : e.clientY;
      const move = (ev) => {
        const cur = isV ? ev.clientX : ev.clientY;
        onDelta(cur - last);
        last = cur;
      };
      const up = () => {
        el.classList.remove('dragging');
        document.body.classList.remove('splitter-dragging');
        el.removeEventListener('pointermove', move);
        el.removeEventListener('pointerup', up);
        el.removeEventListener('pointercancel', up);
        save();
      };
      el.addEventListener('pointermove', move);
      el.addEventListener('pointerup', up);
      el.addEventListener('pointercancel', up);
    });
    el.addEventListener('keydown', (e) => {
      const step = e.shiftKey ? 48 : 16;
      const grow = isV ? (e.key === 'ArrowLeft') : (e.key === 'ArrowUp');
      const shrink = isV ? (e.key === 'ArrowRight') : (e.key === 'ArrowDown');
      if (grow)  { onDelta(-step); save(); e.preventDefault(); }
      if (shrink){ onDelta(+step); save(); e.preventDefault(); }
    });
  }

  // Dragging left (negative dx) grows the inspector; up (negative dy) grows output.
  wireSplitter($('split-insp'), (dx) => { st.inspW = clampInsp(st.inspW - dx); apply(); });
  wireSplitter($('split-out'),  (dy) => { st.outH  = clampOut(st.outH - dy);  apply(); });

  const toggleInsp = () => { st.inspCollapsed = !st.inspCollapsed; apply(); save(); };
  const toggleOut  = () => { st.outCollapsed  = !st.outCollapsed;  apply(); save(); };

  // Global shortcuts: ] toggles inspector, \\ toggles output — but not
  // while typing in an input/textarea/select.
  document.addEventListener('keydown', (e) => {
    const t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT')) return;
    if (e.key === ']') { toggleInsp(); e.preventDefault(); }
    if (e.key === '\\') { toggleOut(); e.preventDefault(); }
  });

  $('btn-collapse-insp').addEventListener('click', toggleInsp);
  $('btn-collapse-out').addEventListener('click', toggleOut);
  $('split-insp').addEventListener('dblclick', toggleInsp);
  $('split-out').addEventListener('dblclick', toggleOut);
  const inspStrip = document.querySelector('.panel-strip-v');
  inspStrip.addEventListener('click', toggleInsp);
  inspStrip.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') { toggleInsp(); e.preventDefault(); }
  });
  const outStrip = document.querySelector('.panel-strip-h');
  outStrip.addEventListener('click', toggleOut);
  outStrip.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') { toggleOut(); e.preventDefault(); }
  });

  window.addEventListener('resize', () => {
    st.inspW = clampInsp(st.inspW);
    st.outH  = clampOut(st.outH);
    apply();
  });

  load();
  apply();
  return { toggleInsp, toggleOut };
})();

// Single global AgentMeta listener: when async agent metadata arrives,
// re-render the inspector if the selected node is an agent.
AgentMeta.onChange(() => {
  const n = S.nodes.find(x => x.id === S.selected);
  if (n && n.type === 'agent') renderInspector();
});

// ─── Init ────────────────────────────────────────────────────────────
loadSampleGraph();
render();
refreshWorkflowList();
log('Loaded sample loop demo. Right-click the canvas to add nodes, scroll to zoom, middle-drag to pan.');
