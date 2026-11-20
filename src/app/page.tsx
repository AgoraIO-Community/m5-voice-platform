import PocketHome from "@/components/pocket-home";
import { authMode, getSessionUser } from "@/lib/auth";

export default async function Home({ searchParams }: {
  searchParams: Promise<{ login?: string; authError?: string }>;
}) {
  const [user, query] = await Promise.all([getSessionUser(), searchParams]);
  return <PocketHome authenticated={Boolean(user)} mode={authMode()}
    promptLogin={query.login === "1" || Boolean(query.authError)}
    initialError={query.authError ? "Sign-in could not be completed. Please try again." : undefined} />;
}
