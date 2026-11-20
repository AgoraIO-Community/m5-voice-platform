import { createHash, createHmac, randomBytes, randomInt, timingSafeEqual } from "node:crypto";
import { AgoraClient, Area, generateConvoAIToken, V3Error } from "agora-agents";
import { engineJoinBody, joinStickEngine, leaveStickEngine } from "@/lib/stick-engine.mjs";
import { getAssignedDetails, getDeviceLibrary, syncDeviceLibrary } from "@/lib/device-assignment";
import {
  MAX_RTC_UID,
  createRtcClientToken,
  stopRtcAgent,
  type RtcCredentials,
} from "@/lib/rtc-sessions";

class DeviceError extends Error {
  constructor(readonly status: number, message: string) { super(message); }
}

function matches(a: string, b: string) {
  const hash = (value: string) => createHash("sha256").update(value).digest();
  return timingSafeEqual(hash(a), hash(b));
}

function respond(status: number, msg: string, data?: object) {
  return Response.json({ code: status === 200 ? 0 : status, msg, data }, {
    status, headers: { "Cache-Control": "private, no-store" },
  });
}

export function deviceRoute(action: (req: Request, credentials: RtcCredentials) => Promise<object | undefined>) {
  return async (req: Request) => {
    const key = process.env.VOICE_DEVICE_KEY;
    if (!key) return respond(503, "Device access is not configured.");
    if (!matches(req.headers.get("authorization") ?? "", "Bearer " + key)) {
      return respond(401, "Invalid device credentials.");
    }
    const appId = process.env.AGORA_APP_ID;
    const appCertificate = process.env.AGORA_APP_CERTIFICATE;
    if (!appId || !appCertificate) return respond(503, "RTC project credentials are missing.");
    try {
      return respond(200, "success", await action(req, { appId, appCertificate }));
    } catch (error) {
      if (error instanceof DeviceError) return respond(error.status, error.message);
      if (error instanceof V3Error) {
        console.warn("[device-rtc]", error.statusCode, error.reason, error.requestId);
      } else if (error instanceof Error) {
        console.warn("[device-rtc]", error.name, error.message.replace(/[A-Za-z0-9_+/=-]{48,}/g, "[redacted]").slice(0, 400));
      }
      return respond(502, "The device session service could not complete the request.");
    }
  };
}

// Engine v2 accepts signed 32-bit participant IDs.
const MAX_ENGINE_UID = 0x7fffffff;

function validUid(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value > 0 && value <= MAX_RTC_UID;
}

function validChannel(value: unknown): value is string {
  return typeof value === "string" && /^[A-Za-z0-9_-]{1,63}$/.test(value);
}

async function body(req: Request) {
  const input = await req.json().catch(() => null);
  if (!input || typeof input !== "object" || Array.isArray(input)) {
    throw new DeviceError(400, "Invalid JSON body.");
  }
  return input;
}

export const deviceAgents = deviceRoute(async (req) => {
  const deviceId = req.headers.get("X-Device-Id");
  if (deviceId === null) return getDeviceLibrary();
  if (!/^[0-9A-F]{12}$/.test(deviceId)) throw new DeviceError(400, "Invalid Stick device ID.");
  return syncDeviceLibrary(deviceId);
});

async function assignment(agentId?: unknown) {
  const { agents } = await getDeviceLibrary();
  if (!agents.length) throw new DeviceError(409, "Add an agent to the Stick first.");
  if (agentId === undefined) return agents[0];
  if (typeof agentId !== "string" || !agentId) throw new DeviceError(400, "Invalid stored agent ID.");
  const selected = agents.find((agent) => agent.id === agentId);
  if (!selected) throw new DeviceError(409, "This agent is no longer on the Stick.");
  return selected;
}

export const deviceConfig = deviceRoute(async (req, credentials) => {
  await assignment();
  const params = new URL(req.url).searchParams;
  const prefix = params.get("channel") || "stick";
  if (!/^[A-Za-z0-9_-]{1,16}$/.test(prefix)) throw new DeviceError(400, "Invalid channel prefix.");
  const requested = params.get("uid");
  if (requested !== null && !/^-?[0-9]+$/.test(requested)) {
    throw new DeviceError(400, "Invalid RTC UID.");
  }
  const parsed = Number(requested);
  if (!Number.isSafeInteger(parsed) || parsed > MAX_ENGINE_UID) {
    throw new DeviceError(400, "Invalid RTC UID.");
  }
  const uid = parsed > 0 ? parsed : randomInt(1, MAX_ENGINE_UID + 1);
  const agentUid = uid === MAX_ENGINE_UID ? 1 : uid + 1;
  const name = [prefix, Math.floor(Date.now() / 1000).toString(36), randomBytes(8).toString("hex")].join("-");
  const channel = name + "-" + signature(`channel:${name}:${uid}:${agentUid}`, credentials);
  const token = createRtcClientToken(credentials, channel, uid);
  return { app_id: credentials.appId, channel_name: channel, uid, agent_uid: agentUid, token };
});

// The board treats the returned handle as opaque. A signed handle scopes stop
// requests to sessions created by this device API across serverless instances.
function signature(id: string, credentials: RtcCredentials) {
  return createHmac("sha256", credentials.appCertificate)
    .update("stick-session:" + id).digest("base64url").slice(0, 22);
}

export const startDeviceAgent = deviceRoute(async (req, credentials) => {
  const { channelName, userUid, rtcUid, agentId, parameters } = await body(req);
  if (!validChannel(channelName) || !validUid(userUid) || !validUid(rtcUid) || rtcUid === userUid) {
    throw new DeviceError(400, "Invalid RTC channel or UIDs.");
  }
  const name = channelName.slice(0, -23);
  const issuedAt = parseInt(name.split("-").at(-2) ?? "", 36);
  const age = Math.floor(Date.now() / 1000) - issuedAt;
  const signedChannel = name + "-" + signature(`channel:${name}:${userUid}:${rtcUid}`, credentials);
  if (!Number.isFinite(age) || age < 0 || age > 600 || !matches(channelName, signedChannel)) {
    throw new DeviceError(400, "Invalid or expired device channel.");
  }
  const selected = await assignment(agentId);
  const codec = parameters?.output_audio_codec ?? "g722";
  if (codec !== "g722" && codec !== "pcma") throw new DeviceError(400, "Invalid Stick output codec.");
  const details = await getAssignedDetails(selected.id, () =>
    new AgoraClient({ area: Area.US, ...credentials }).v3.agents.get({ id: selected.id }));
  const token = generateConvoAIToken({ ...credentials, channelName, uid: rtcUid, tokenExpire: 3600, privilegeExpire: 3600 });
  const join = engineJoinBody(details, { channel: channelName, userUid, rtcUid, token, codec });
  const sessionId = await joinStickEngine(credentials.appId, token, join);
  console.info("Stick engine start succeeded", { agentId: selected.id, sessionId, agentRtcUid: rtcUid, codec });
  const handleId = "v2-" + sessionId;
  return {
    agent_id: handleId + "." + signature(handleId, credentials),
    channel_name: channelName, status: "started",
  };
});

export const stopDeviceAgent = deviceRoute(async (req, credentials) => {
  const { agentId } = await body(req);
  if (typeof agentId !== "string") throw new DeviceError(400, "Invalid session handle.");
  const [id, sig, extra] = agentId.split(".");
  if (!id || !sig || extra !== undefined || !matches(sig, signature(id, credentials))) {
    throw new DeviceError(400, "Invalid session handle.");
  }
  if (id.startsWith("v2-")) {
    const authToken = generateConvoAIToken({
      ...credentials, channelName: "stick-control", uid: 0, tokenExpire: 60, privilegeExpire: 60,
    });
    await leaveStickEngine(credentials.appId, authToken, id.slice(3));
  } else {
    await stopRtcAgent(credentials, id); // Handles issued before the v2 migration.
  }
});
