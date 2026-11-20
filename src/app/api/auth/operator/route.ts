import { NextResponse } from "next/server";

import {
  authMode,
  OPERATOR_USER,
  setSessionCookie,
  signSession,
  verifyOperatorKey,
} from "@/lib/auth";

export const runtime = "nodejs";

export async function POST(request: Request) {
  if (authMode() !== "operator") {
    return NextResponse.json({ error: "Operator login is disabled." }, { status: 404 });
  }

  const contentType = request.headers.get("content-type")?.split(";")[0].trim();
  if (contentType !== "application/json") {
    return NextResponse.json({ error: "Expected a JSON request." }, { status: 415 });
  }
  const body = await request.json().catch(() => null);
  if (typeof body?.key !== "string") {
    return NextResponse.json({ error: "An access key is required." }, { status: 400 });
  }

  const valid = verifyOperatorKey(body.key);
  if (valid === null) {
    return NextResponse.json({ error: "Operator login is unavailable." }, { status: 503 });
  }
  if (!valid) {
    return NextResponse.json({ error: "Invalid access key." }, { status: 401 });
  }

  await setSessionCookie(await signSession(OPERATOR_USER));
  return NextResponse.json({ ok: true });
}
