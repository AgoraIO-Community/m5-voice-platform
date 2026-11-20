// The Agora Platform preview supports these managed OpenAI models for a saved agent.
export const STICK_MODELS = ["gpt-4.1-mini", "gpt-4o-mini", "gpt-5-mini", "gpt-5-nano", "gpt-5.6-luna"];
export const STICK_VOICES = ["alloy", "ash", "coral", "echo", "fable", "nova", "onyx", "sage", "shimmer"];

/** @param {{ name: string, prompt: string, greeting: string, mode: string, model: string, voice: string }} form */
export function buildStickAgentConfig({ name, prompt, greeting, mode, model, voice }) {
  const agentName = name.trim();
  if (!agentName || Array.from(agentName).length > 255) {
    throw new Error("Enter an agent name between 1 and 255 characters.");
  }
  const instructions = prompt.trim();
  if (!instructions) throw new Error("Enter instructions for the agent.");
  if (mode !== "cascade") {
    throw new Error("Agora Platform supports only cascaded pipelines for saved agents.");
  }
  if (!STICK_MODELS.includes(model) || !STICK_VOICES.includes(voice)) {
    throw new Error("Choose a supported OpenAI model and voice.");
  }
  return {
    agent_name: agentName,
    instructions,
    ...(greeting.trim() && { greeting: { mode: "text", text: greeting.trim() } }),
    pipeline: {
      mode: "cascaded",
      asr: { credential: { mode: "managed" }, vendor: "ares", language: "en-US" },
      llm: { credential: { mode: "managed" }, vendor: "openai", style: "openai", model },
      tts: { credential: { mode: "managed" }, vendor: "openai", params: { model: "tts-1", voice } },
    },
  };
}
