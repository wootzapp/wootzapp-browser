#!/usr/bin/env node
import { runGatewayTask } from '../lib/gateway-task-runner.mjs';
import { runOpenAITask } from '../lib/openai-task-runner.mjs';

function usage() {
  console.log(`agent-browser for Wootz Chromium desktop

Usage:
  agent-browser task "go to https://example.com and summarize the page"
  agent-browser "go to https://example.com and summarize the page"

Environment:
  OPENAI_API_KEY             Use OpenAI SDK task mode
  OPENAI_MODEL               Required model name for OpenAI task mode
  AI_GATEWAY_API_KEY         Optional Vercel AI Gateway task mode
  AI_GATEWAY_MODEL           Optional AI Gateway model name
  AGENT_BROWSER_CDP_TARGET   Browser CDP endpoint, default http://127.0.0.1:9222
  AGENT_BROWSER_MAX_STEPS    Required positive step limit for OpenAI task mode
`);
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length === 0 || args.includes('--help') || args.includes('-h')) {
    usage();
    return 0;
  }
  if (args[0] === '--version' || args[0] === 'version') {
    console.log('wootzapp desktop agent-browser');
    return 0;
  }
  const task = args[0] === 'task' ? args.slice(1).join(' ').trim() : args.join(' ').trim();
  if (!task) {
    throw new Error('task instruction is required');
  }
  if (process.env.OPENAI_API_KEY) {
    return await runOpenAITask(task);
  }
  if (process.env.AI_GATEWAY_API_KEY) {
    return runGatewayTask(task);
  }
  throw new Error('OPENAI_API_KEY or AI_GATEWAY_API_KEY is required for task mode');
}

main()
  .then((code) => process.exit(code))
  .catch((error) => {
    console.error(`agent-browser: ${error.message}`);
    process.exit(1);
  });
