import assert from "node:assert/strict";
import test from "node:test";
import { buildStickAgentConfig } from "../src/lib/stick-agent-config.mjs";

const form = { name: " Pocket ", prompt: " Be concise ", greeting: " Hi! ", mode: "cascade", model: "gpt-4.1-mini", voice: "nova" };

test("maps the form to the saved agent's managed stages", () => {
  const agent = buildStickAgentConfig(form);
  assert.equal(agent.agent_name, "Pocket");
  assert.equal(agent.instructions, "Be concise");
  assert.deepEqual(agent.greeting, { mode: "text", text: "Hi!" });
  assert.deepEqual(agent.pipeline, {
    mode: "cascaded",
    asr: { credential: { mode: "managed" }, vendor: "ares", language: "en-US" },
    llm: { credential: { mode: "managed" }, vendor: "openai", style: "openai", model: form.model },
    tts: { credential: { mode: "managed" }, vendor: "openai", params: { model: "tts-1", voice: "nova" } },
  });
});

test("omits an empty optional greeting", () => {
  const agent = buildStickAgentConfig({ ...form, greeting: " " });
  assert.equal("greeting" in agent, false);
});

test("rejects unsupported pipelines, models, voices and invalid names", () => {
  for (const change of [{ prompt: "  " }, { mode: "mllm" }, { model: "gpt-realtime" }, { voice: "invalid" }, { name: " " }, { name: "x".repeat(256) }]) {
    assert.throws(() => buildStickAgentConfig({ ...form, ...change }));
  }
});
