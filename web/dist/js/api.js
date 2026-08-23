// js/api.js — thin fetch wrappers for the HiveMind REST API.
export const API = {
  // Phase 9: every /api/* call needs a bearer key. Stored in localStorage
  // after a one-time prompt (web UI is the localhost fallback client).
  getKey() {
    let k = localStorage.getItem('hivemind_api_key');
    if (!k) {
      k = window.prompt('HiveMind API key (printed once at server start):', '');
      if (k) localStorage.setItem('hivemind_api_key', k.trim());
    }
    return k || '';
  },
  async req(method, path, body) {
    const opt = { method, headers: {} };
    if (path.startsWith('/api/') && path !== '/api/health')
      opt.headers['Authorization'] = 'Bearer ' + this.getKey();
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
