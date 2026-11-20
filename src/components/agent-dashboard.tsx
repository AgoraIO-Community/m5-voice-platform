"use client";

import { useEffect, useRef, useState } from "react";
import {
  AudioLines,
  Minus,
  Upload,
  LogOut,
  Plus,
  RefreshCw,
} from "lucide-react";

import { Alert, AlertDescription } from "@/components/ui/alert";
import { BrowserAgentTest } from "./browser-agent-test";
import { StickAgentForm } from "@/components/stick-agent-form";
import Link from "next/link";
import type { AuthMode, SessionUser } from "@/lib/auth";
import { Button } from "@/components/ui/button";
import styles from "./agent-dashboard.module.css";
import { apiError, createStoredAgent, v3ListAll } from "@/components/v3-api";
import type { buildStickAgentConfig } from "@/lib/stick-agent-config.mjs";

export default function AgentDashboard({user, mode}: {user: SessionUser; mode: AuthMode}) {
  const [authError, setAuthError] = useState("");
  async function handleSignOut() {
    if (mode === "bypass") return;
    if (mode === "operator") {
      try {
        const res = await fetch("/api/auth/operator/logout", { method: "POST" });
        if (!res.ok) throw new Error("Sign-out failed");
        window.location.replace("/");
      } catch {
        setAuthError("Couldn’t sign out. Please try again.");
      }
      return;
    }
    try {
      const res = await fetch("/api/auth/agora/logout", { method: "POST" });
      const data = (await res.json()) as { redirect?: string };
      window.location.href =
        typeof data.redirect === "string" ? data.redirect : "/";
    } catch {
      window.location.href = "/";
    }
  }

  return (
    <div className={styles.page}>
      <header className={styles.header}>
        <Link href="/" className={styles.brand}><span><AudioLines size={22} aria-hidden /></span>PocketFullOfAgents</Link>
        <div className={styles.account}>
          <span>{user.email || user.name || user.id}</span>
          {mode !== "bypass" && <Button variant="ghost" size="icon" onClick={handleSignOut} aria-label="Sign out"><LogOut size={16} /></Button>}
        </div>
      </header>
      <main className={styles.main}>
        {authError && <Alert variant="destructive"><AlertDescription>{authError}</AlertDescription></Alert>}
        <AgentLibrary />
      </main>
    </div>
  );
}

type StoredAgent = { agent_id: string; agent_name?: string; greeting?: { text?: string } | null };

type DeviceLibrary = {
  agents: { id: string; name: string }[];
  deviceId?: string | null;
  account: string;
};

async function readDeviceLibrary(response: Response): Promise<DeviceLibrary> {
  const body = await response.json().catch(() => null);
  if (!response.ok) throw new Error(apiError(body, "Couldn’t access the device library."));
  if (
    Array.isArray(body?.agents)
    && body.agents.every((agent: DeviceLibrary["agents"][number]) =>
      typeof agent?.id === "string" && typeof agent?.name === "string")
    && typeof body.account === "string"
    && (body.deviceId == null || typeof body.deviceId === "string")
  ) return body;
  throw new Error("The server returned an invalid device library.");
}

function AgentLibrary() {
  const [testAgent, setTestAgent] = useState<{ id: string; name: string } | null>(null);
  const [agents, setAgents] = useState<StoredAgent[]>([]);
  const [deviceLibrary, setDeviceLibrary] = useState<DeviceLibrary | null>(null);
  const [savingId, setSavingId] = useState<string | null>(null);
  const saving = savingId !== null;
  const [saveError, setSaveError] = useState("");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [deviceError, setDeviceError] = useState("");
  const [revision, setRevision] = useState(0);
  const [showCreateForm, setShowCreateForm] = useState(false);
  const [creating, setCreating] = useState(false);
  const [createdName, setCreatedName] = useState("");
  const createInFlight = useRef(false);
  const saveInFlight = useRef(false);
  const unlistedAgents = useRef(new Map<string, StoredAgent>());

  useEffect(() => {
    let active = true;
    Promise.allSettled([
      v3ListAll<StoredAgent>("agents"),
      fetch("/api/device/assignment", { cache: "no-store" }).then(readDeviceLibrary),
    ]).then(([list, library]) => {
      if (!active) return;
      if (list.status === "fulfilled") {
        const uniqueAgents = new Map(list.value.map((agent) => [agent.agent_id, agent]));
        // Preserve a successful create if the list hasn't caught up yet.
        for (const [id, agent] of unlistedAgents.current) {
          if (uniqueAgents.has(id)) unlistedAgents.current.delete(id);
          else uniqueAgents.set(id, agent);
        }
        setAgents([...uniqueAgents.values()]);
      } else {
        setError(list.reason instanceof Error ? list.reason.message : "Couldn’t load stored agents.");
      }
      if (library.status === "fulfilled") {
        setDeviceLibrary(library.value);
      } else {
        setDeviceError(library.reason instanceof Error ? library.reason.message : "Couldn’t load the device library.");
      }
      setLoading(false);
    });
    return () => { active = false; };
  }, [revision]);

  async function createAgent(config: ReturnType<typeof buildStickAgentConfig>) {
    if (createInFlight.current || loading || saving) return;
    createInFlight.current = true;
    setCreating(true);
    setCreatedName("");
    try {
      const agentId = await createStoredAgent(config);
      const agentName = config.agent_name;
      const agent = { agent_id: agentId, agent_name: agentName, greeting: config.greeting };
      unlistedAgents.current.set(agentId, agent);
      setAgents((list) => [agent, ...list.filter((item) => item.agent_id !== agentId)]);
      setSaveError("");
      setCreatedName(agentName);
      setShowCreateForm(false);
    } finally {
      createInFlight.current = false;
      setCreating(false);
    }
  }

  async function saveMembership(agentId: string, remove: boolean) {
    if (saveInFlight.current || createInFlight.current || loading || deviceError || !deviceLibrary) return;
    saveInFlight.current = true;
    setSavingId(agentId);
    setSaveError("");
    try {
      const response = await fetch("/api/device/assignment", {
        method: remove ? "DELETE" : "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ agentId }),
      });
      const library = await readDeviceLibrary(response);
      setDeviceLibrary(library);
      if (library.agents.some((agent) => agent.id === agentId) === remove) {
        throw new Error("The server didn’t confirm the device library update.");
      }
      setCreatedName("");
    } catch (err) {
      setSaveError(err instanceof Error ? err.message : "Couldn’t update the device library.");
    } finally {
      saveInFlight.current = false;
      setSavingId(null);
    }
  }

  const deviceAgents = deviceLibrary?.agents ?? [];
  const deviceAgentIds = new Set(deviceAgents.map((agent) => agent.id));
  const listedAgentIds = new Set(agents.map((agent) => agent.agent_id));
  const rows = [
    ...agents,
    ...deviceAgents.filter((agent) => !listedAgentIds.has(agent.id)).map((agent): StoredAgent => ({
      agent_id: agent.id,
      agent_name: agent.name,
    })),
  ];

  return (
    <>
      <div className={styles.intro}>
        <div>
          <h1>Your agents</h1>
          <p>Choose which agents appear on your Stick.</p>
        </div>
        <button className={styles.newAgent} disabled={showCreateForm || saving || loading}
          aria-controls={showCreateForm ? "stick-agent-create" : undefined} aria-expanded={showCreateForm}
          onClick={() => { setCreatedName(""); setShowCreateForm(true); }}>
          <Plus size={17} aria-hidden /> New agent
        </button>
      </div>
      {showCreateForm && <StickAgentForm pending={creating} disabled={loading || saving} onCancel={() => setShowCreateForm(false)} onCreate={createAgent} />}
      <div className={styles.workspace}>
        <section className={styles.library} aria-labelledby="agent-library">
          <div className={styles.libraryHeader}>
            <h2 id="agent-library">Agent library</h2>
            <button className={styles.refresh} disabled={loading || saving || creating} onClick={() => {
              setLoading(true);
              setError("");
              setDeviceError("");
              setSaveError("");
              setCreatedName("");
              setRevision((value) => value + 1);
            }}><RefreshCw size={15} aria-hidden /> Refresh</button>
          </div>
          {error && <Alert variant="destructive"><AlertDescription>{error}</AlertDescription></Alert>}
          {deviceError && <Alert variant="destructive"><AlertDescription>{deviceError}</AlertDescription></Alert>}
          {saveError && <Alert variant="destructive"><AlertDescription>{saveError}</AlertDescription></Alert>}
          {createdName && <p role="status" className={styles.message}>{createdName} was added to your library.</p>}
          {loading && <p role="status" className={styles.message}>Loading agents…</p>}
          {!loading && !error && !rows.length && <p className={styles.message}>Your library is empty. Create an agent to get started.</p>}
          {testAgent && <BrowserAgentTest key={testAgent.id} agentId={testAgent.id} name={testAgent.name} onClose={() => setTestAgent(null)} />}
          <ul className={styles.list}>
            {rows.map((agent) => {
              const name = agent.agent_name || agent.agent_id;
              const onDevice = deviceAgentIds.has(agent.agent_id);
              const updating = savingId === agent.agent_id;
              return (
                <li key={agent.agent_id} className={styles.row}>
                  <span aria-hidden className={styles.initial}>{name.slice(0, 1)}</span>
                  <div className={styles.agent}>
                    <h3>{name}</h3>
                    {agent.greeting?.text && <p>{agent.greeting.text}</p>}
                  </div>
                  <div className={styles.rowActions}>
                  <button type="button" className={styles.push} disabled={!!testAgent}
                    aria-label={"Test " + name + " in browser"}
                    onClick={() => setTestAgent({ id: agent.agent_id, name })}>
                    Test in browser
                  </button>
                  <button type="button" className={styles.push} data-on-device={onDevice}
                    aria-label={onDevice ? "Remove " + name + " from device" : "Push " + name + " to device"}
                    disabled={saving || creating || loading || !!deviceError || !deviceLibrary}
                    onClick={() => saveMembership(agent.agent_id, onDevice)}>
                    {onDevice ? <Minus size={15} aria-hidden /> : <Upload size={15} aria-hidden />}
                    {onDevice ? (updating ? "Removing…" : "Remove from device") : (updating ? "Pushing…" : "Push to device")}
                  </button>
                  </div>
                </li>
              );
            })}
          </ul>
        </section>
        <aside className={styles.device} aria-labelledby="stick-heading">
          <div className={styles.devicePhoto}><img src="/stick-pocket.webp" alt="M5StickS3" width="170" height="196" /></div>
          <div className={styles.deviceText}>
            <h2 id="stick-heading">Your Stick</h2>
            <p>Demo account <strong>{deviceLibrary?.account ?? "hermes"}</strong></p>
            <p className={styles.deviceId} role="status">
              {loading ? "Checking device…" : deviceError ? "Device library unavailable." :
                deviceLibrary?.deviceId ? <>Device ID <code>{deviceLibrary.deviceId}</code></> : "Awaiting device sync"}
            </p>
            {deviceLibrary && !deviceError && (
              <p role="status">{deviceAgents.length} {deviceAgents.length === 1 ? "agent" : "agents"} in the device library</p>
            )}
          </div>
        </aside>
      </div>
    </>
  );
}
