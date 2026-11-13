import type { Agent } from '../shared/contracts.js';

export const avatarNames = [
  'ant', 'beetle', 'blue-butterfly', 'bumblebee', 'butterfly', 'caterpillar', 'cricket',
  'dragonfly', 'drone-bee', 'firefly', 'grasshopper', 'honey-bee', 'ladybug', 'mantis',
  'moth', 'queen-bee', 'snail', 'spider', 'worker-bee', 'worm',
] as const;

export function avatarFor(name: string, stored = ''): string {
  const clean = stored.replace(/\.svg$/i, '');
  const selected = avatarNames.includes(clean as typeof avatarNames[number])
    ? clean : avatarNames[hash(name) % avatarNames.length];
  return `./assets/avatars/${selected}.svg`;
}

export function userAvatar(stored: string): string {
  return stored && avatarNames.includes(stored as typeof avatarNames[number])
    ? `./assets/avatars/${stored}.svg` : './assets/avatars/user.svg';
}

export function agentAvatar(agent: Agent): string { return avatarFor(agent.name, agent.avatar); }

function hash(value: string): number {
  let result = 2166136261;
  for (const character of value) {
    result ^= character.codePointAt(0) ?? 0;
    result = Math.imul(result, 16777619);
  }
  return result >>> 0;
}
