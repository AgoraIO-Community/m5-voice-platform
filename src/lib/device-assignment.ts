import { BlobPreconditionFailedError, get, put } from "@vercel/blob";

import type { AgoraClient } from "agora-agents";

type AgentDetails = Awaited<ReturnType<AgoraClient["v3"]["agents"]["get"]>>;
export type DeviceAgent = { id: string; name: string };

export class DeviceLibraryTooLarge extends Error {}

class DeviceLibraryBusy extends Error {}

type Library = { agents: (DeviceAgent & { details?: AgentDetails })[]; deviceId?: string };

function assignmentPath() {
  const project = process.env.AGORA_APP_ID;
  if (!project) throw new Error("The Agora project is missing.");
  return `devices/sticks3/${encodeURIComponent(project)}/assignment.json`;
}

async function readAssignment(): Promise<{ library: Library; etag?: string }> {
  const blob = await get(assignmentPath(), { access: "private", useCache: false });
  if (!blob) return { library: { agents: [] } };
  if (blob.statusCode !== 200 || !blob.blob.etag) throw new Error("Could not read the device assignment.");
  const stored = await new Response(blob.stream).json();
  let agents: Library["agents"];
  if (Array.isArray(stored?.agents) && stored.agents.every((agent: DeviceAgent | null) =>
    typeof agent?.id === "string" && agent.id && typeof agent.name === "string" && agent.name
  )) {
    agents = stored.agents;
  } else if (typeof stored?.agentId === "string" && stored.agentId && stored.agents === undefined) {
    agents = [{ id: stored.agentId, name: stored.agentId }];
  } else {
    throw new Error("Invalid stored device assignment.");
  }
  const deviceId = stored.deviceId;
  if (deviceId !== undefined && (typeof deviceId !== "string" || !/^[0-9A-F]{12}$/.test(deviceId))) {
    throw new Error("Invalid stored Stick ID.");
  }
  return { library: { agents, deviceId }, etag: blob.blob.etag };
}

function publicLibrary(library: Library) {
  return { ...library, agents: library.agents.map(({ id, name }) => ({ id, name })), account: "hermes" };
}

export async function getAssignedDetails(id: string, load: () => Promise<AgentDetails>) {
  const selected = (await readAssignment()).library.agents.find(agent => agent.id === id);
  if (!selected) throw new Error("Agent removed from Stick.");
  if (selected.details) return selected.details;
  const fetched = await load();
  const library = await changeLibrary(current => {
    const entry = current.agents.find(agent => agent.id === id);
    if (!entry) throw new Error("Agent removed from Stick.");
    if (entry.details) return current;
    return { ...current, agents: current.agents.map(agent => agent.id === id ? { ...agent, details: fetched } : agent) };
  }, 1).catch(async error => {
    if (!(error instanceof DeviceLibraryBusy)) throw error;
    // Backfilling an older assignment is optional for the current call.
    return (await readAssignment()).library;
  });
  const assigned = library.agents.find(agent => agent.id === id);
  if (!assigned) throw new Error("Agent removed from Stick.");
  return assigned.details ?? fetched;
}

export async function getDeviceLibrary() {
  const { library } = await readAssignment();
  return publicLibrary(library);
}

async function changeLibrary(change: (library: Library) => Library, attempts = 8) {
  for (let attempt = 0; attempt < attempts; attempt++) {
    const { library, etag } = await readAssignment();
    const next = change(library);
    if (next === library) return library;
    try {
      await put(assignmentPath(), JSON.stringify(next), {
        access: "private",
        addRandomSuffix: false,
        allowOverwrite: Boolean(etag),
        ifMatch: etag,
        contentType: "application/json",
      });
      return next;
    } catch (error) {
      // Simultaneous first writes use create-only mode. Check whether another
      // writer created the library before repeating the merge.
      if (!(error instanceof BlobPreconditionFailedError) && (etag || !(await readAssignment()).etag)) throw error;
    }
  }
  throw new DeviceLibraryBusy("The device library is busy.");
}

export function syncDeviceLibrary(deviceId: string) {
  return changeLibrary((library) => library.deviceId === deviceId ? library : { ...library, deviceId }).then(publicLibrary);
}

export function updateDeviceLibrary(id: string, name: string | null, details?: AgentDetails) {
  return changeLibrary((library) => {
    const existing = library.agents.find((agent) => agent.id === id);
    if (name === null) {
      if (!existing) return library;
      return { ...library, agents: library.agents.filter((agent) => agent.id !== id) };
    }
    if (existing?.name === name && !details) return library;
    const agents = existing
      ? library.agents.map((agent) => agent.id === id ? { id, name, details } : agent)
      : [...library.agents, { id, name, details }];
    // Reserve space for the hardware ID even before the board's first sync.
    const data = publicLibrary({ agents, deviceId: "000000000000" });
    if (Buffer.byteLength(JSON.stringify({ code: 0, msg: "success", data })) > 16 * 1024) {
      throw new DeviceLibraryTooLarge("The Stick library is full. Remove an agent before adding another.");
    }
    return { ...library, agents };
  }).then(publicLibrary);
}
