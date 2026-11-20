"use client";

import { type FormEvent, useState } from "react";

import { AgentTextField } from "@/components/agent-text-field";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Label } from "@/components/ui/label";
import { buildStickAgentConfig, STICK_MODELS, STICK_VOICES } from "@/lib/stick-agent-config.mjs";

const selectClassName = "border-input bg-background h-9 w-full rounded-md border px-3 text-sm";

type Props = {
  pending: boolean;
  disabled: boolean;
  onCancel: () => void;
  onCreate: (config: ReturnType<typeof buildStickAgentConfig>) => Promise<void>;
};

export function StickAgentForm({ pending, disabled, onCancel, onCreate }: Props) {
  const [error, setError] = useState("");

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (pending || disabled) return;
    setError("");
    const fields = new FormData(event.currentTarget);

    try {
      const config = buildStickAgentConfig({
        name: String(fields.get("name") ?? ""),
        prompt: String(fields.get("prompt") ?? ""),
        greeting: String(fields.get("greeting") ?? ""),
        mode: String(fields.get("pipeline") ?? ""),
        model: String(fields.get("model") ?? ""),
        voice: String(fields.get("voice") ?? ""),
      });
      await onCreate(config);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Couldn’t create the agent.");
    }
  }

  return (
    <Card id="stick-agent-create">
      <CardHeader>
        <CardTitle>Create an agent</CardTitle>
      </CardHeader>
      <CardContent>
        <form onSubmit={submit} className="space-y-4" aria-busy={pending}>
          <fieldset disabled={pending} className="space-y-4">
            <AgentTextField kind="name" id="stick-agent-name" />
            <AgentTextField kind="prompt" id="stick-agent-prompt" controlClassName="min-h-28" />
            <AgentTextField kind="greeting" id="stick-agent-greeting" />
            {/* Reveal the selector when saved agents support a second pipeline. */}
            <div hidden className="space-y-2">
              <Label htmlFor="stick-agent-pipeline">Pipeline</Label>
              <select
                id="stick-agent-pipeline"
                name="pipeline"
                aria-describedby="stick-agent-pipeline-hint"
                className={selectClassName}
                defaultValue="cascade"
              >
                <option value="cascade">ARES speech recognition → OpenAI text model → OpenAI TTS</option>
                <option value="live" disabled>GPT Live MLLM (unavailable for saved agents)</option>
              </select>
              <p id="stick-agent-pipeline-hint" className="text-muted-foreground text-sm">
                Agora Platform currently supports cascaded pipelines for saved agents. GPT Live requires a realtime pipeline.
              </p>
            </div>
            <div className="grid gap-4 sm:grid-cols-2">
              <div className="space-y-2">
                <Label htmlFor="stick-agent-model">OpenAI model</Label>
                <select id="stick-agent-model" name="model" className={selectClassName}>
                  {STICK_MODELS.map((model) => <option key={model} value={model}>{model}</option>)}
                </select>
              </div>
              <div className="space-y-2">
                <Label htmlFor="stick-agent-voice">OpenAI voice</Label>
                <select id="stick-agent-voice" name="voice" className={selectClassName}>
                  {STICK_VOICES.map((voice) => <option key={voice} value={voice}>{voice}</option>)}
                </select>
              </div>
            </div>
          </fieldset>
          {error && <Alert variant="destructive"><AlertDescription>{error}</AlertDescription></Alert>}
          <div className="flex flex-wrap gap-2">
            <Button type="submit" disabled={pending || disabled}>{pending ? "Creating…" : "Create agent"}</Button>
            <Button type="button" variant="outline" disabled={pending} onClick={onCancel}>Cancel</Button>
          </div>
        </form>
      </CardContent>
    </Card>
  );
}
