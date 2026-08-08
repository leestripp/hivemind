// js/api.js — thin fetch wrappers for the HiveMind REST API.
export const API = {
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
