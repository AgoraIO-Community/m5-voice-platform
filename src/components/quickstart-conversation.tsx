"use client";

import { useEffect, useState } from "react";
import AgoraRTC, { AgoraRTCProvider, RemoteUser, useClientEvent, useJoin, useLocalMicrophoneTrack, usePublish, useRemoteUsers, useRTCClient } from "agora-rtc-react";
import { AgoraVoiceAI, AgoraVoiceAIEvents, TranscriptHelperMode } from "agora-agent-client-toolkit";
import type { RTMClient } from "agora-rtm";
import type { Ticket } from "./browser-agent-test";

type Props = { ticket: Ticket; rtm: RTMClient; name: string; onEnd(): void };

export default function Conversation(props: Props) {
  const [client] = useState(() => AgoraRTC.createClient({ mode: "rtc", codec: "vp8" }));
  return <AgoraRTCProvider client={client}><AudioSession {...props} /></AgoraRTCProvider>;
}

function AudioSession({ ticket, rtm, name, onEnd }: Props) {
  const client = useRTCClient();
  const [ready, setReady] = useState(false);
  const [state, setState] = useState("");
  const [issue, setIssue] = useState("");
  const [needsGesture, setNeedsGesture] = useState(false);
  const [transcript, setTranscript] = useState<{ speaker: string; text: string }[]>([]);
  useEffect(() => {
    const timer = setTimeout(() => setReady(true), 0);
    return () => clearTimeout(timer);
  }, []);
  const join = useJoin({ appid: ticket.appId, channel: ticket.channel, token: ticket.token, uid: ticket.uid }, ready);
  const microphone = useLocalMicrophoneTrack(ready);
  const publication = usePublish([microphone.localMicrophoneTrack]);
  const agent = useRemoteUsers().find(user => String(user.uid) === String(ticket.agentUid));
  useClientEvent(client, "user-left", user => { if (String(user.uid) === String(ticket.agentUid)) onEnd(); });
  useEffect(() => {
    const blocked = () => setNeedsGesture(true);
    AgoraRTC.on("autoplay-failed", blocked);
    return () => { AgoraRTC.off("autoplay-failed", blocked); };
  }, []);
  useEffect(() => {
    if (!join.isConnected) return;
    let active = true;
    let instance: Awaited<ReturnType<typeof AgoraVoiceAI.init>> | undefined;
    async function subscribe() {
      const ai = await AgoraVoiceAI.init({ rtcEngine: client, rtmEngine: rtm, renderMode: TranscriptHelperMode.TEXT });
      if (!active) { ai.destroy(); return; }
      instance = ai;
      ai.on(AgoraVoiceAIEvents.TRANSCRIPT_UPDATED, items => setTranscript(items.map(item => ({
        speaker: String(item.uid) === String(ticket.agentUid) ? name : "You", text: item.text ?? "",
      }))));
      ai.on(AgoraVoiceAIEvents.AGENT_STATE_CHANGED, (_, event) => setState(String(event.state)));
      ai.on(AgoraVoiceAIEvents.AGENT_ERROR, (_, error) => setIssue(error.message));
      await ai.subscribeMessage(ticket.channel);
    }
    void subscribe().catch(() => { if (active) setIssue("Conversation events unavailable."); });
    return () => { active = false; instance?.unsubscribe(); instance?.destroy(); };
  }, [client, join.isConnected, rtm, ticket, name]);

  const error = join.error || microphone.error || publication.error;
  let status = "Connecting browser audio…";
  if (join.isConnected) {
    if (!agent) status = "Browser connected. Waiting for the agent to join…";
    else if (!agent.hasAudio) status = "Agent joined. Waiting for it to publish audio…";
    else status = state || "Agent audio published.";
  }
  return <>
    <p role="status">{status}</p>
    {(error || issue) && <p role="alert">{error?.message || issue}</p>}
    {agent && <RemoteUser user={agent} playVideo={false} />}
    {needsGesture && <button onClick={() => { agent?.audioTrack?.play(); setNeedsGesture(false); }}>Enable sound</button>}
    {transcript.length > 0 && <div aria-label="Live transcript">{transcript.map((turn, index) =>
      <p key={index}><strong>{turn.speaker}</strong> {turn.text}</p>)}</div>}
  </>;
}
