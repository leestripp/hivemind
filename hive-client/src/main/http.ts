export interface HttpResponse {
  status: number;
  bytes: ArrayBuffer;
  contentType: string;
  headers: Headers;
}

export async function requestBytes(
  base: string, path: string, apiKey: string, init: RequestInit = {}, timeoutMs = 30_000,
): Promise<HttpResponse> {
  const url = `${base.replace(/\/+$/, '')}${path}`;
  const parsed = new URL(url);
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') throw new Error('Unsupported service URL');
  const headers = new Headers(init.headers);
  if (apiKey) headers.set('Authorization', `Bearer ${apiKey}`);
  headers.set('Accept', headers.get('Accept') ?? 'application/json');
  let response: Response;
  try {
    response = await fetch(parsed, { ...init, headers, cache: 'no-store', signal: AbortSignal.timeout(timeoutMs) });
  } catch (error) {
    const message = error instanceof Error && error.name === 'TimeoutError'
      ? `Request timed out after ${Math.round(timeoutMs / 1000)} seconds`
      : `Could not reach ${parsed.host}`;
    throw new Error(message, { cause: error });
  }
  return {
    status: response.status, bytes: await response.arrayBuffer(),
    contentType: response.headers.get('content-type') ?? '', headers: response.headers,
  };
}

function cleanErrorBody(body: string): string {
  const text = body.replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ').trim();
  return text.slice(0, 240) || 'empty response';
}

export function responseError(status: number, bytes: ArrayBuffer, fallback: string): Error {
  const body = new TextDecoder().decode(bytes);
  let message = fallback;
  try {
    const root = JSON.parse(body) as { error?: unknown; request_id?: unknown };
    if (typeof root.error === 'string') message = root.error;
    else if (root.error && typeof root.error === 'object' && 'message' in root.error &&
      typeof root.error.message === 'string') message = root.error.message;
    if (typeof root.request_id === 'string' && root.request_id) message += ` [${root.request_id}]`;
  } catch {
    if (body) message = `${fallback}: ${cleanErrorBody(body)}`;
  }
  return new Error(`${message} (HTTP ${status})`);
}

export async function requestJson(
  base: string, path: string, apiKey: string, method = 'GET', body?: unknown, timeoutMs = 30_000,
): Promise<unknown> {
  const headers: Record<string, string> = {};
  const init: RequestInit = { method, headers };
  if (body !== undefined || method === 'POST' || method === 'PATCH') {
    headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body ?? {});
  }
  const response = await requestBytes(base, path, apiKey, init, timeoutMs);
  if (response.status < 200 || response.status >= 300) {
    throw responseError(response.status, response.bytes, `${method} ${path} failed`);
  }
  try { return JSON.parse(new TextDecoder().decode(response.bytes)) as unknown; }
  catch { throw new Error(`Service returned invalid JSON (HTTP ${response.status})`); }
}

export function discoveryIds(value: unknown, description: string): string[] {
  if (!value || typeof value !== 'object' || !('data' in value) || !Array.isArray(value.data)) {
    throw new Error(`ChimpFlow returned an invalid ${description} list`);
  }
  return value.data.flatMap((entry: unknown) => {
    if (!entry || typeof entry !== 'object' || !('id' in entry) || typeof entry.id !== 'string' || !entry.id) return [];
    return [entry.id];
  });
}
