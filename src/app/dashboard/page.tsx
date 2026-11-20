import { redirect } from "next/navigation";
import AgentDashboard from "@/components/agent-dashboard";
import { authMode, getSessionUser } from "@/lib/auth";

export const metadata = { title: "Agent dashboard · PocketFullOfAgents" };

export default async function Dashboard() {
  const user = await getSessionUser();
  if (!user) redirect("/?login=1");
  return <AgentDashboard user={user} mode={authMode()} />;
}
