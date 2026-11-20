import { createHmac, randomBytes, timingSafeEqual } from "node:crypto";

import { RtcRole, RtcTokenBuilder } from "agora-token";
import { getSessionUser } from "@/lib/auth";
import {
  MAX_RTC_UID,
  nextRtcUid,
  randomRtcUid,
  startRtcAgent,
  stopRtcAgent,
  type RtcCredentials,
} from "@/lib/rtc-sessions";
import { getV3Credentials, v3ErrorJson } from "@/lib/v3-client";

export const runtime = "nodejs";

const CHANNEL_START_WINDOW_SECONDS = 120;
const SESSION_LIFECYCLE = { idle_timeout_ms: 120_000, max_duration_ms: 300_000 };
const PRIVATE_HEADERS = { "Cache-Control": "private, no-store" };

function json(data: object, status = 200) {
  return Response.json(data, { status, headers: PRIVATE_HEADERS });
}

function signedScope(
  credentials: RtcCredentials,
  userId: string,
  purpose: "channel" | "stop",
  ...values: string[]
) {
  return createHmac("sha256", credentials.appCertificate)
    .update(JSON.stringify(["browser-agent-test", credentials.appId, userId, purpose, ...values]))
    .digest("base64url")
    .slice(0, 22);
}

function validSignature(received: string, expected: string) {
  return /^[A-Za-z0-9_-]{22}$/.test(received)
    && timingSafeEqual(Buffer.from(received), Buffer.from(expected));
}

type TestAction = (
  input: Record<string, unknown>,
  credentials: RtcCredentials,
  userId: string,
) => Response | Promise<Response>;

function testRoute(action: TestAction) {
  return async (request: Request) => {
    const user = await getSessionUser();
    if (!user) return json({ error: "Sign in first." }, 401);
    if (request.headers.get("content-type")?.split(";")[0].trim() !== "application/json") {
      return json({ error: "Expected a JSON request." }, 415);
    }
    const input = await request.json().catch(() => null);
    if (!input || typeof input !== "object" || Array.isArray(input)) {
      return json({ error: "Invalid JSON body." }, 400);
    }
    try {
      return await action(input, await getV3Credentials(), user.id);
    } catch (error) {
      const response = v3ErrorJson(error);
      response.headers.set("Cache-Control", PRIVATE_HEADERS["Cache-Control"]);
      return response;
    }
  };
}

function storedAgentId(value: unknown) {
  return typeof value === "string" && value.length <= 256 ? value.trim() : "";
}

export const POST = testRoute(({ agentId: inputAgentId }, credentials, userId) => {
  const agentId = storedAgentId(inputAgentId);
  if (!agentId) return json({ error: "Select a saved agent." }, 400);

  const now = Math.floor(Date.now() / 1000).toString(36);
  const uid = randomRtcUid();
  const name = `test-${now}-${randomBytes(8).toString("hex")}-${uid.toString(36)}`;
  const channel = `${name}-${signedScope(credentials, userId, "channel", name, agentId)}`;
  return json({
    appId: credentials.appId,
    channel,
    uid,
    agentUid: nextRtcUid(uid),
    token: RtcTokenBuilder.buildTokenWithRtm(
      credentials.appId, credentials.appCertificate, channel, String(uid), RtcRole.PUBLISHER, 600, 600,
    ),
  });
});

export const PUT = testRoute(async ({ agentId: inputAgentId, channel }, credentials, userId) => {
  const agentId = storedAgentId(inputAgentId);
  if (!agentId) return json({ error: "Select a saved agent." }, 400);

  const match = typeof channel === "string"
    ? /^(test-([0-9a-z]{1,8})-[0-9a-f]{16}-([0-9a-z]{1,7}))-([A-Za-z0-9_-]{22})$/.exec(channel)
    : null;
  if (!match) return json({ error: "Invalid test channel." }, 400);
  const [channelName, name, issuedAt, uidText, signature] = match;
  const uid = parseInt(uidText, 36);
  const age = Math.floor(Date.now() / 1000) - parseInt(issuedAt, 36);
  if (uid < 1 || uid > MAX_RTC_UID || age < 0 || age > CHANNEL_START_WINDOW_SECONDS
      || !validSignature(signature, signedScope(credentials, userId, "channel", name, agentId))) {
    return json({ error: "Invalid or expired test channel." }, 400);
  }

  const sessionId = await startRtcAgent(credentials, {
    agentId,
    channelName,
    userUid: uid,
    rtcUid: nextRtcUid(uid),
    idempotencyKey: channelName,
    lifecycle: SESSION_LIFECYCLE,
  });
  if (!sessionId) return json({ error: "Agora did not return a test session ID." }, 502);
  return json({ agentId, sessionId, handle: `${sessionId}.${signedScope(credentials, userId, "stop", sessionId)}` });
});

export const DELETE = testRoute(async ({ handle }, credentials, userId) => {
  const [id, signature, extra] = typeof handle === "string" && handle.length <= 512
    ? handle.split(".")
    : [];
  if (!id || !signature || extra !== undefined
      || !validSignature(signature, signedScope(credentials, userId, "stop", id))) {
    return json({ error: "Invalid test session handle." }, 400);
  }
  await stopRtcAgent(credentials, id);
  return json({ ok: true });
});
