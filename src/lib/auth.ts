import { createHash, createHmac, timingSafeEqual } from "node:crypto";
import { SignJWT, jwtVerify } from "jose";
import { cookies } from "next/headers";

/** Synthetic identity when AUTH_MODE=bypass (local dev only). */
export const DEV_USER = {
  id: "dev@local",
  email: "dev@local",
  name: "Local Dev",
} as const;

export const OPERATOR_USER = {
  id: "operator@local",
  email: "operator@local",
  name: "Operator",
} as const;

export type SessionUser = {
  id: string;
  email: string;
  name: string;
};

export type AuthMode = "operator" | "sso" | "bypass";

export const SESSION_COOKIE = "pstn_session";
export const OAUTH_STATE_COOKIE = "pstn_oauth_state";
export const PROJECT_COOKIE = "pstn_project";

const SESSION_MAX_AGE_SECONDS = 60 * 60 * 24;
const OAUTH_STATE_MAX_AGE_SECONDS = 60 * 10;

function getOperatorKey(): string | null {
  const key = process.env.OPERATOR_ACCESS_KEY;
  return key && key.length >= 32 ? key : null;
}

export function verifyOperatorKey(candidate: string): boolean | null {
  const key = getOperatorKey();
  if (!key) return null;
  const expected = createHash("sha256").update(key).digest();
  const received = createHash("sha256").update(candidate).digest();
  return timingSafeEqual(expected, received);
}

function deriveOperatorSecret(key: string, purpose: string): Uint8Array {
  return createHmac("sha256", key).update(purpose).digest();
}

function getSessionSecret(mode: AuthMode): Uint8Array {
  const raw = process.env.SESSION_JWT_SECRET;
  if (mode === "operator") {
    const key = getOperatorKey();
    if (!key) {
      throw new Error("Operator access is not configured.");
    }
    const signingKey = raw && raw.length >= 32 ? raw : key;
    return createHmac("sha256", signingKey)
      .update("m5-voice-platform:operator-session:")
      .update(key)
      .digest();
  }
  if (!raw || raw.length < 32) {
    throw new Error(
      "SESSION_JWT_SECRET must be set to at least 32 characters (use a random hex string).",
    );
  }
  return new TextEncoder().encode(raw);
}

function getProjectCookieSecret(): Uint8Array {
  const raw = process.env.SESSION_JWT_SECRET || process.env.AGORA_CUSTOMER_SECRET;
  if (!raw && authMode() === "operator") {
    const key = getOperatorKey();
    if (key) return deriveOperatorSecret(key, "m5-voice-platform:operator-project");
  }
  if (!raw) {
    throw new Error(
      "SESSION_JWT_SECRET or AGORA_CUSTOMER_SECRET is required for project selection.",
    );
  }
  return new TextEncoder().encode(raw);
}

function production(): boolean {
  return (process.env.APP_ENV ?? process.env.NODE_ENV) === "production";
}

/**
 * Production defaults to operator access and local development defaults to bypass.
 * Set AUTH_MODE=sso to enable Agora OAuth.
 */
export function authMode(): AuthMode {
  const fallback = production() ? "operator" : "bypass";
  const raw = process.env.AUTH_MODE?.trim().toLowerCase() || fallback;
  if (raw === "bypass") {
    if (production()) {
      const allow = (process.env.ALLOW_BYPASS_IN_PRODUCTION || "")
        .trim()
        .toLowerCase();
      if (allow !== "true") {
        return "operator";
      }
      console.warn(
        "[auth] AUTH_MODE=bypass is active in production because ALLOW_BYPASS_IN_PRODUCTION=true.",
      );
    }
    return "bypass";
  }
  return raw === "sso" ? "sso" : "operator";
}

export async function signSession(user: SessionUser): Promise<string> {
  const mode = authMode();
  const now = Math.floor(Date.now() / 1000);
  const claims = { email: user.email, name: user.name };
  return await new SignJWT(
    mode === "operator" ? { ...claims, scope: "operator-session" } : claims,
  )
    .setProtectedHeader({ alg: "HS256", typ: "JWT" })
    .setSubject(user.id)
    .setIssuedAt(now)
    .setExpirationTime(now + SESSION_MAX_AGE_SECONDS)
    .sign(getSessionSecret(mode));
}

export async function verifySession(
  token: string,
): Promise<SessionUser | null> {
  try {
    const mode = authMode();
    const { payload } = await jwtVerify(token, getSessionSecret(mode), {
      algorithms: ["HS256"],
      requiredClaims: ["sub", "exp"],
    });
    if (mode === "operator") {
      return payload.scope === "operator-session" && payload.sub === OPERATOR_USER.id
        ? { ...OPERATOR_USER }
        : null;
    }
    if (payload.scope !== undefined) return null;
    if (!payload.sub) return null;
    return {
      id: String(payload.sub),
      email: typeof payload.email === "string" ? payload.email : "",
      name: typeof payload.name === "string" ? payload.name : "",
    };
  } catch {
    return null;
  }
}

export async function getSessionUser(): Promise<SessionUser | null> {
  if (authMode() === "bypass") {
    return { ...DEV_USER };
  }
  const store = await cookies();
  const token = store.get(SESSION_COOKIE)?.value;
  if (!token) return null;
  return await verifySession(token);
}

export async function setSessionCookie(token: string): Promise<void> {
  const store = await cookies();
  store.set(SESSION_COOKIE, token, {
    httpOnly: true,
    secure: production(),
    sameSite: "lax",
    path: "/",
    maxAge: SESSION_MAX_AGE_SECONDS,
  });
}

export async function clearSessionCookie(): Promise<void> {
  const store = await cookies();
  store.delete(SESSION_COOKIE);
  store.delete(PROJECT_COOKIE);
}

export async function setProjectCookie(appId: string): Promise<void> {
  const now = Math.floor(Date.now() / 1000);
  const token = await new SignJWT({ scope: "agora-project" })
    .setProtectedHeader({ alg: "HS256", typ: "JWT" })
    .setSubject(appId)
    .setIssuedAt(now)
    .setExpirationTime(now + SESSION_MAX_AGE_SECONDS)
    .sign(getProjectCookieSecret());
  const store = await cookies();
  store.set(PROJECT_COOKIE, token, {
    httpOnly: true,
    secure: production(),
    sameSite: "lax",
    path: "/",
    maxAge: SESSION_MAX_AGE_SECONDS,
  });
}

export async function getProjectAppId(): Promise<string | null> {
  const store = await cookies();
  const token = store.get(PROJECT_COOKIE)?.value;
  if (!token) return null;
  try {
    const { payload } = await jwtVerify(token, getProjectCookieSecret(), {
      algorithms: ["HS256"],
    });
    return payload.scope === "agora-project" && payload.sub
      ? String(payload.sub)
      : null;
  } catch {
    return null;
  }
}

export async function setOAuthStateCookie(state: string): Promise<void> {
  const store = await cookies();
  store.set(OAUTH_STATE_COOKIE, state, {
    httpOnly: true,
    secure: production(),
    sameSite: "lax",
    path: "/",
    maxAge: OAUTH_STATE_MAX_AGE_SECONDS,
  });
}

export async function consumeOAuthStateCookie(): Promise<string | null> {
  const store = await cookies();
  const current = store.get(OAUTH_STATE_COOKIE)?.value ?? null;
  store.delete(OAUTH_STATE_COOKIE);
  return current;
}
