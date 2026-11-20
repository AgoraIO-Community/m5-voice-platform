"use client";

import { useState } from "react";
import type { AuthMode } from "@/lib/auth";
import PocketLanding from "@/components/pocket-landing";
import OperatorLoginDialog from "@/components/operator-login-dialog";

export default function PocketHome({ authenticated, mode, promptLogin, initialError }: {
  authenticated: boolean;
  mode: AuthMode;
  promptLogin: boolean;
  initialError?: string;
}) {
  const [open, setOpen] = useState(promptLogin && !authenticated);
  const [error, setError] = useState(initialError);

  function dismiss() {
    setOpen(false);
    setError(undefined);
    if (promptLogin) window.history.replaceState(null, "", "/");
  }

  return (
    <>
      <PocketLanding authenticated={authenticated} onLogin={() => setOpen(true)} />
      {open && <OperatorLoginDialog authMode={mode} onClose={dismiss} initialError={error} />}
    </>
  );
}
