"use client";

import dynamic from "next/dynamic";
import { useEffect, useState } from "react";
import type { RTMClient } from "agora-rtm";
import { apiError } from "./v3-api";
import styles from "./agent-dashboard.module.css";

const Conversation = dynamic(() => import("./quickstart-conversation"), { ssr: false });

async function request<T>(method: string, body: Record<string, string>): Promise<T> {
  const response = await fetch("/api/agent-test", {
    method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    keepalive: method === "DELETE",
  });
  const data = await response.json().catch(() => null);
  if (!response.ok) throw new Error(apiError(data, "Couldn’t complete the browser test."));
  return data as T;
}

export type Ticket = { appId: string; channel: string; uid: number; token: string; agentUid: number };

export function BrowserAgentTest({ agentId, name, onClose }: { agentId: string; name: string; onClose(): void }) {
  const [session, setSession] = useState<{ ticket: Ticket; rtm: RTMClient; sessionId: string }>();
  const [error, setError] = useState("");
  useEffect(() => {
    let active = true;
    let handle: string | undefined;
    let rtm: RTMClient | undefined;
    function stop() {
      active = false;
      if (handle) {
        void request("DELETE", { handle }).catch(() => setError("Couldn’t confirm the agent stopped."));
        handle = undefined;
      }
      if (rtm) {
        void rtm.logout().catch(() => {});
        rtm = undefined;
      }
    }
    async function start() {
      try {
        const ticket = await request<Ticket>("POST", { agentId });
        const { default: AgoraRTM } = await import("agora-rtm");
        if (!active) return;
        const signaling = new AgoraRTM.RTM(ticket.appId, String(ticket.uid));
        await signaling.login({ token: ticket.token });
        rtm = signaling;
        if (!active) { stop(); return; }
        await signaling.subscribe(ticket.channel);
        if (!active) return;
        const started = await request<{ handle: string; agentId: string; sessionId: string }>("PUT", { agentId, channel: ticket.channel });
        handle = started.handle;
        if (!active) { stop(); return; }
        console.info("Agora agent start succeeded", { agentId: started.agentId, sessionId: started.sessionId, agentRtcUid: ticket.agentUid });
        setSession({ ticket, rtm: signaling, sessionId: started.sessionId });
      } catch (cause) {
        if (active) setError(cause instanceof Error ? cause.message : "Couldn’t connect to Agora.");
        stop();
      }
    }
    const timer = window.setTimeout(() => void start(), 0);
    window.addEventListener("pagehide", stop);
    return () => { clearTimeout(timer); window.removeEventListener("pagehide", stop); stop(); };
  }, [agentId]);

  return <section className={styles.testPanel}>
    <div><strong>Browser test · {name}</strong>
      {session && <p>Saved agent ID <code>{agentId}</code><br />Session ID <code>{session.sessionId}</code><br />Agent RTC UID <code>{session.ticket.agentUid}</code></p>}
      {error ? <p role="alert">{error}</p> : session
        ? <Conversation {...session} name={name} onEnd={onClose} />
        : <p role="status">Connecting…</p>}
    </div>
    <button className={styles.push} onClick={onClose}>{error ? "Close" : "End test"}</button>
  </section>;
}
