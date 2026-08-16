import { spawnSync } from 'node:child_process';

export function coreCommand() {
  return process.env.AGENT_BROWSER_CORE_COMMAND || '/usr/local/bin/agent-browser-core';
}

export function cdpTarget() {
  return process.env.AGENT_BROWSER_CDP_TARGET || process.env.AGENT_BROWSER_CDP_URL || '9222';
}

export function runCore(args, { json = true, allowFailure = false } = {}) {
  const finalArgs = ['--cdp', cdpTarget(), ...args];
  if (json && !finalArgs.includes('--json')) {
    finalArgs.push('--json');
  }
  const result = spawnSync(coreCommand(), finalArgs, {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '1' },
    maxBuffer: 20 * 1024 * 1024,
  });
  const stdout = (result.stdout || '').trim();
  const stderr = (result.stderr || '').trim();
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0 && !allowFailure) {
    throw new Error(stderr || stdout || `agent-browser-core exited with status ${result.status}`);
  }
  if (!json) {
    return stdout;
  }
  try {
    return JSON.parse(stdout);
  } catch (error) {
    throw new Error(`agent-browser-core returned invalid JSON: ${stdout.slice(0, 2000)}`);
  }
}

export function normalizeRef(value) {
  const ref = String(value || '').trim();
  if (!ref) return '';
  return ref.startsWith('@') ? ref : `@${ref}`;
}
