import { spawnSync } from 'node:child_process';
import { cdpTarget, coreCommand } from './agent-browser-core.mjs';

export function runGatewayTask(task) {
  const modelArgs = process.env.AI_GATEWAY_MODEL ? ['--model', process.env.AI_GATEWAY_MODEL] : [];
  const result = spawnSync(coreCommand(), ['--cdp', cdpTarget(), ...modelArgs, 'chat', task], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '1' },
    stdio: ['ignore', 'inherit', 'inherit'],
  });
  if (result.error) {
    throw result.error;
  }
  return result.status || 0;
}
