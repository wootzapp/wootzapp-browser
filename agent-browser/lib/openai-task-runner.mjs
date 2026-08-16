import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { normalizeRef, runCore } from './agent-browser-core.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const promptPath = resolve(here, '../prompts/desktop-task.md');
const SYSTEM_PROMPT = readFileSync(promptPath, 'utf8');

function maxSteps() {
  const raw = Number.parseInt(process.env.AGENT_BROWSER_MAX_STEPS || '20', 10);
  return Number.isFinite(raw) && raw > 0 ? raw : 20;
}

function modelName() {
  return process.env.OPENAI_MODEL || process.env.AGENT_BROWSER_MODEL || 'gpt-4o';
}

function snapshotText(payload) {
  const data = payload && typeof payload === 'object' ? payload.data : null;
  if (data && typeof data.snapshot === 'string') {
    return data.snapshot;
  }
  return JSON.stringify(payload, null, 2);
}

function parseDecision(content) {
  const text = String(content || '').trim();
  const match = text.match(/```(?:json)?\s*([\s\S]*?)```/i);
  const jsonText = match ? match[1].trim() : text;
  const parsed = JSON.parse(jsonText);
  if (!parsed || typeof parsed !== 'object' || typeof parsed.action !== 'string') {
    throw new Error('model response must be a JSON object with an action field');
  }
  return parsed;
}

async function decide(client, task, observation, history) {
  const messages = [
    { role: 'system', content: SYSTEM_PROMPT },
    {
      role: 'user',
      content: [
        `Task: ${task}`,
        '',
        'Latest browser snapshot:',
        observation,
        '',
        'Recent action history:',
        history.length ? history.slice(-8).join('\n') : 'None',
      ].join('\n'),
    },
  ];
  const response = await client.chat.completions.create({
    model: modelName(),
    messages,
    temperature: 0,
    response_format: { type: 'json_object' },
  });
  return parseDecision(response.choices?.[0]?.message?.content || '');
}

function executeDecision(decision) {
  const action = decision.action;
  if (action === 'open' || action === 'navigate') {
    if (!decision.url) throw new Error('open requires url');
    return runCore(['open', String(decision.url)]);
  }
  if (action === 'click') {
    const ref = normalizeRef(decision.ref || decision.id);
    if (!ref) throw new Error('click requires ref');
    return runCore(['click', ref]);
  }
  if (action === 'fill') {
    const ref = normalizeRef(decision.ref || decision.id);
    if (!ref) throw new Error('fill requires ref');
    return runCore(['fill', ref, String(decision.text ?? '')]);
  }
  if (action === 'type') {
    const ref = normalizeRef(decision.ref || decision.id);
    if (!ref) throw new Error('type requires ref');
    return runCore(['type', ref, String(decision.text ?? '')]);
  }
  if (action === 'press') {
    if (!decision.key) throw new Error('press requires key');
    return runCore(['press', String(decision.key)]);
  }
  if (action === 'wait') {
    const ms = Number.isFinite(Number(decision.milliseconds)) ? Number(decision.milliseconds) : 1000;
    return runCore(['wait', String(Math.max(0, Math.round(ms)))]);
  }
  if (action === 'scroll') {
    const direction = String(decision.direction || 'down');
    const pixels = Number.isFinite(Number(decision.pixels)) ? Number(decision.pixels) : 600;
    return runCore(['scroll', direction, String(Math.max(1, Math.round(pixels)))]);
  }
  throw new Error(`unsupported action: ${action}`);
}

export async function runOpenAITask(task) {
  if (!process.env.OPENAI_API_KEY) {
    throw new Error('OPENAI_API_KEY is required for OpenAI task mode');
  }
  const { default: OpenAI } = await import('openai');
  const client = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });
  const history = [];
  for (let step = 1; step <= maxSteps(); step += 1) {
    const snapshot = runCore(['snapshot', '-i']);
    const observation = snapshotText(snapshot);
    const decision = await decide(client, task, observation, history);
    if (decision.action === 'final') {
      const answer = String(decision.answer || '').trim();
      console.log(answer || 'Done.');
      return 0;
    }
    const result = executeDecision(decision);
    history.push(`${step}. ${JSON.stringify(decision)} -> ${JSON.stringify(result).slice(0, 500)}`);
  }
  throw new Error(`task did not finish within AGENT_BROWSER_MAX_STEPS=${maxSteps()}`);
}
