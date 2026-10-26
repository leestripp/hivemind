// Shared Hive-garden avatar selection and GTK-compatible name hashing.
import { esc } from './util.js';
// ── auto avatar — mirrors the GTK client exactly ──────────────────────
// Client: auto_avatar(name) = AVATARS[std::hash<string>(name) % 20], and
// libstdc++ std::hash<string> is MurmurHash64A(seed 0xc70f6907).
// Reimplemented here (BigInt) so the web picks the same department head
// the client shows in the roster/bubbles. Order must match the client's
// avatar_names() (department pairs: male, then female).
const AVATARS = [
  // Hive-garden creature set (hexagon badges) — bees first.
  // Order must match the GTK client's avatar_names().
  'queen-bee', 'worker-bee', 'drone-bee', 'honey-bee', 'bumblebee',
  'ladybug', 'butterfly', 'blue-butterfly', 'dragonfly', 'firefly',
  'caterpillar', 'snail', 'ant', 'beetle', 'spider',
  'moth', 'grasshopper', 'worm', 'mantis', 'cricket',
];

function murmur64a(str) {
  const m = 0xc6a4a7935bd1e995n, r = 47n, mask = 0xffffffffffffffffn;
  const bytes = new TextEncoder().encode(str);
  let h = (0xc70f6907n ^ (BigInt(bytes.length) * m)) & mask;
  const nBlocks = bytes.length >> 3;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, nBlocks * 8);
  for (let i = 0; i < nBlocks; i++) {
    let k = dv.getBigUint64(i * 8, true);           // little-endian
    k = (k * m) & mask; k ^= k >> r; k = (k * m) & mask;
    h ^= k; h = (h * m) & mask;
  }
  // Tail: XOR each remaining byte at its little-endian position, ONE multiply
  // (mirrors the C++ switch-fallthrough in MurmurHash64A).
  const tail = bytes.slice(nBlocks * 8);
  for (let i = 0; i < tail.length; i++)
    h ^= BigInt(tail[i]) << BigInt(8 * i);
  if (tail.length) h = (h * m) & mask;
  h ^= h >> r; h = (h * m) & mask; h ^= h >> r;
  return h;
}

// "Auto" avatar for agents/templates without an explicit choice: stable
// name hash into the dept-head set (same pick as the client).
const autoAvatar = (name) => AVATARS[Number(murmur64a(name) % BigInt(AVATARS.length))];
// Stale stored choices (old department set) fall back to the hash pick —
// a missing file must never render a broken image.
const avatarSrc = (t) => {
  const a = t.avatar && AVATARS.includes(t.avatar) ? t.avatar : autoAvatar(t.name);
  return '/avatars/' + esc(a) + '.svg';
};


export { AVATARS, autoAvatar, avatarSrc };
