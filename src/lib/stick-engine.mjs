// Agora Engine v2 expects provider settings inline on each join.
export function engineJoinBody(agent, { channel, userUid, rtcUid, token, codec }) {
  const { asr, llm, tts } = agent.pipeline ?? {};
  if (!llm || !tts || [asr, llm, tts].some(stage => stage && stage.credential?.mode !== "managed")) {
    throw new Error("Stick playback requires a managed speech pipeline.");
  }
  if (!["g722", "pcma"].includes(codec)) throw new Error("Unsupported Stick output codec.");

  const systemMessages = [...(agent.messages ?? [])];
  if (agent.instructions) systemMessages.unshift({ role: "system", content: agent.instructions });
  return {
    name: channel,
    properties: {
      channel, token, agent_rtc_uid: String(rtcUid), remote_rtc_uids: [String(userUid)],
      enable_string_uid: false, idle_timeout: 30,
      advanced_features: { enable_rtm: true },
      parameters: {
        output_audio_codec: codec, audio_scenario: "chorus", data_channel: "rtm",
        enable_error_message: true, enable_metrics: true,
      },
      ...(asr?.language && { turn_detection: { language: asr.language } }),
      // ARES is the v2 default. Its locale belongs in turn_detection.
      ...(asr?.vendor === "ares" && { asr: { vendor: "ares" } }),
      ...(asr && asr.vendor !== "ares" && { asr: {
        vendor: asr.vendor, credential_mode: "managed",
        params: { ...asr.params, ...(asr.language && { language: asr.language }), ...(asr.model && { model: asr.model }) },
      } }),
      llm: {
        vendor: llm.vendor, style: llm.style, credential_mode: "managed",
        url: llm.url ?? (llm.vendor === "openai" ? "https://api.openai.com/v1/chat/completions" : undefined),
        params: { ...llm.params, model: llm.model }, system_messages: systemMessages,
        greeting_message: agent.greeting?.text, failure_message: llm.failure_message,
        max_history: llm.max_history,
      },
      tts: {
        vendor: tts.vendor,
        credential_mode: "managed",
        params: {
          ...tts.params,
          url: tts.url ?? tts.params?.url ?? (tts.vendor === "openai" ? "https://api.openai.com/v1/audio/speech" : undefined),
        },
      },
    },
  };
}

function engineRequest(appId, authToken, path, method, body) {
  return fetch(`https://api.agora.io/api/conversational-ai-agent/v2/projects/${encodeURIComponent(appId)}/${path}`, {
    method, cache: "no-store", redirect: "error", signal: AbortSignal.timeout(15000),
    headers: { Authorization: `agora token="${authToken}"`, "Content-Type": "application/json" },
    ...(body && { body: JSON.stringify(body) }),
  });
}

export async function joinStickEngine(appId, authToken, body) {
  const response = await engineRequest(appId, authToken, "join", "POST", body);
  if (response.status === 409) {
    // The engine requires a unique agent name. Recover an active start on this signed channel.
    const query = new URLSearchParams({ channel: body.properties.channel, state: "1,2" });
    const found = await engineRequest(appId, authToken, "agents?" + query, "GET");
    if (found.ok) {
      const page = await found.json();
      const agents = page.data?.list;
      if (agents?.length === 1 && typeof agents[0].agent_id === "string" && agents[0].agent_id) {
        return agents[0].agent_id;
      }
    }
  }
  if (!response.ok) {
    const failure = await response.json().catch(() => ({}));
    const info = [failure.reason, failure.detail].filter(value => typeof value === "string").join(" ");
    throw new Error(`Agora Engine join failed (HTTP ${response.status}). ${info}`);
  }
  const { agent_id } = await response.json();
  if (typeof agent_id !== "string" || !agent_id) throw new Error("Agora Engine returned no runtime ID.");
  return agent_id;
}

export async function leaveStickEngine(appId, authToken, id) {
  const response = await engineRequest(appId, authToken, `agents/${encodeURIComponent(id)}/leave`, "POST");
  if (!response.ok && response.status !== 404) throw new Error(`Agora Engine leave failed (HTTP ${response.status}).`);
}
