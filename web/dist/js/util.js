// js/util.js — shared DOM/format helpers.
export const $ = (id) => document.getElementById(id);

export function esc(s) {
  return String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}


export function log(msg) {
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

