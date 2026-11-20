import assert from "node:assert/strict";
import test from "node:test";
import { buildStickAgentConfig } from "../src/lib/stick-agent-config.mjs";
import { engineJoinBody, joinStickEngine, leaveStickEngine } from "../src/lib/stick-engine.mjs";

const riff = buildStickAgentConfig({
  name: "Riff", prompt: "Host a trivia game.", greeting: "It's game time!",
  mode: "cascade", model: "gpt-4o-mini", voice: "echo",
});
const connection = { channel: "stick-test", rtcUid: 2002, userUid: 2001, token: "fixture", codec: "g722" };

test("device join carries the stored persona, selected voice, UIDs and requested codec", () => {
  for (const codec of ["g722", "pcma"]) {
    const { properties } = engineJoinBody(riff, { ...connection, codec });
    assert.equal(properties.parameters.output_audio_codec, codec);
    assert.deepEqual(properties.asr, { vendor: "ares" });
    assert.equal(properties.turn_detection.language, "en-US");
    assert.equal(properties.llm.greeting_message, riff.greeting.text);
    assert.equal(properties.llm.system_messages[0].content, riff.instructions);
    assert.equal(properties.llm.params.model, riff.pipeline.llm.model);
    assert.equal(properties.tts.params.voice, "echo");
    assert.equal(properties.tts.params.url, "https://api.openai.com/v1/audio/speech");
    assert.equal(properties.agent_rtc_uid, "2002");
    assert.deepEqual(properties.remote_rtc_uids, ["2001"]);
  }
});

test("device lifecycle uses engine join and leave, with idempotent cleanup", async t => {
  const fetch = t.mock.method(globalThis, "fetch");
  fetch.mock.mockImplementationOnce(() => Response.json({ agent_id: "engine123" }));
  fetch.mock.mockImplementationOnce(() => new Response(null, { status: 404 }), 1);
  const id = await joinStickEngine("project", "fixture", engineJoinBody(riff, connection));
  assert.equal(id, "engine123");
  await leaveStickEngine("project", "fixture", id);
  assert.deepEqual(fetch.mock.calls.map(call => new URL(call.arguments[0]).pathname), [
    "/api/conversational-ai-agent/v2/projects/project/join",
    "/api/conversational-ai-agent/v2/projects/project/agents/engine123/leave",
  ]);
});

test("a repeated start recovers the sole active engine ID for the signed channel", async t => {
  const fetch = t.mock.method(globalThis, "fetch");
  fetch.mock.mockImplementationOnce(() => new Response(null, { status: 409 }));
  fetch.mock.mockImplementationOnce(() => Response.json({
    data: { list: [{ agent_id: "existing", status: "RUNNING" }] },
  }), 1);
  assert.equal(await joinStickEngine("project", "fixture", engineJoinBody(riff, connection)), "existing");
  const query = new URL(fetch.mock.calls[1].arguments[0]).searchParams;
  assert.equal(query.get("channel"), connection.channel);
  assert.equal(query.get("state"), "1,2");
});
