import { NextResponse } from "next/server";

import { authMode, clearSessionCookie } from "@/lib/auth";

export const runtime = "nodejs";

export async function POST() {
  if (authMode() !== "operator") {
    return NextResponse.json({ error: "Operator login is disabled." }, { status: 404 });
  }

  await clearSessionCookie();
  return NextResponse.json({ ok: true });
}
