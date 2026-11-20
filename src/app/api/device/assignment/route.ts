import { AgoraClient, Area } from "agora-agents";
import { getSessionUser } from "@/lib/auth";
import { DeviceLibraryTooLarge, getDeviceLibrary, updateDeviceLibrary } from "@/lib/device-assignment";
import { v3ErrorJson } from "@/lib/v3-client";

export const runtime = "nodejs";

export async function GET() {
  if (!(await getSessionUser())) return Response.json({ error: "Sign in first." }, { status: 401 });
  try {
    return Response.json(await getDeviceLibrary());
  } catch {
    return Response.json({ error: "Could not read the Stick library." }, { status: 503 });
  }
}

async function changeLibrary(request: Request, add: boolean) {
  if (!(await getSessionUser())) return Response.json({ error: "Sign in first." }, { status: 401 });
  const body = await request.json().catch(() => null);
  if (typeof body?.agentId !== "string" || !body.agentId.trim() || body.agentId.length > 256) {
    return Response.json({ error: "Select a stored agent." }, { status: 400 });
  }
  const id = body.agentId.trim();
  if (add && Buffer.byteLength(id) > 127) {
    return Response.json({ error: "This agent ID exceeds the Stick firmware limit." }, { status: 400 });
  }
  let details: Awaited<ReturnType<AgoraClient["v3"]["agents"]["get"]>> | undefined;
  if (add) {
    // The board always uses the fixed device project, regardless of the Studio project picker.
    const appId = process.env.AGORA_APP_ID;
    const appCertificate = process.env.AGORA_APP_CERTIFICATE;
    if (!appId || !appCertificate) return Response.json({ error: "Device project is unavailable." }, { status: 503 });
    try {
      const client = new AgoraClient({ area: Area.US, appId, appCertificate });
      details = await client.v3.agents.get({ id });
    } catch (error) {
      return v3ErrorJson(error);
    }
  }
  try {
    return Response.json(await updateDeviceLibrary(id, details ? details.agent_name || id : null, details));
  } catch (error) {
    if (error instanceof DeviceLibraryTooLarge) return Response.json({ error: error.message }, { status: 409 });
    return Response.json({ error: "Could not update the Stick library." }, { status: 503 });
  }
}

export function PUT(request: Request) { return changeLibrary(request, true); }
export function DELETE(request: Request) { return changeLibrary(request, false); }
