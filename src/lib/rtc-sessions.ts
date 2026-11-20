import { randomInt } from "node:crypto";
import {
  AgoraClient, Area, generateClientToken, generateConvoAIToken, V3Error,
} from "agora-agents";

export type RtcCredentials = { appId: string; appCertificate: string };
export const MAX_RTC_UID = 0xffffffff;
export function randomRtcUid() {
  return randomInt(1, MAX_RTC_UID + 1);
}

export function nextRtcUid(uid: number) {
  return uid === MAX_RTC_UID ? 1 : uid + 1;
}
const RTC_TOKEN_TTL_SECONDS = 600;

export function createRtcClientToken(credentials: RtcCredentials, channelName: string, uid: number) {
  return generateClientToken({
    ...credentials, channelName, uid,
    tokenExpire: RTC_TOKEN_TTL_SECONDS, privilegeExpire: RTC_TOKEN_TTL_SECONDS,
  });
}

export async function startRtcAgent(credentials: RtcCredentials, {
  agentId, channelName, userUid, rtcUid, idempotencyKey, lifecycle,
}: {
  agentId: string;
  channelName: string;
  userUid: number;
  rtcUid: number;
  idempotencyKey: string;
  lifecycle?: { idle_timeout_ms: number; max_duration_ms: number };
}) {
  const sessions = new AgoraClient({ area: Area.US, ...credentials }).v3.sessions;
  let sessionId: string | undefined;
  try {
    const result = await sessions.start({
      agent_id: agentId,
      idempotencyKey,
      ...(lifecycle && { lifecycle }),
      transport: {
        type: "rtc", channel: channelName, uid: String(rtcUid), subscribe_uids: [String(userUid)],
        token: generateConvoAIToken({
          ...credentials, channelName, uid: rtcUid,
          tokenExpire: RTC_TOKEN_TTL_SECONDS, privilegeExpire: RTC_TOKEN_TTL_SECONDS,
        }),
      },
    });
    sessionId = result.agent_session_id;
  } catch (error) {
    if (!(error instanceof V3Error && error.reason === "IdempotencyKeyConflict")) throw error;
    const page = await sessions.list({ channel: channelName, status: "STARTING,RUNNING", limit: 1 });
    sessionId = page.data?.list?.[0]?.agent_session_id;
  }
  if (sessionId) console.info("Agora agent start succeeded", { agentId, sessionId, agentRtcUid: rtcUid });
  return sessionId;
}

export async function stopRtcAgent(credentials: RtcCredentials, id: string) {
  const sessions = new AgoraClient({ area: Area.US, ...credentials }).v3.sessions;
  try {
    await sessions.stop({ agentSessionId: id });
  } catch (error) {
    if (!(error instanceof V3Error)) throw error;
    if (error.statusCode === 404) return;
    if (error.statusCode !== 409) throw error;
    const session = await sessions.get({ agentSessionId: id });
    if (!["STOPPED", "STOPPING", "FAILED"].includes(session.status ?? "")) throw error;
  }
}
