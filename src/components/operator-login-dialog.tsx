"use client";

import { useEffect, useId, useRef, useState, type FormEvent } from "react";
import Link from "next/link";
import { X } from "lucide-react";

type OperatorLoginDialogProps = {
  onClose: () => void;
  authMode: "operator" | "sso" | "bypass";
  initialError?: string;
};

const actionClass =
  "inline-flex min-h-11 w-full items-center justify-center rounded-lg bg-[#b94016] px-4 py-2.5 font-semibold text-white transition-colors hover:bg-[#a83510] focus-visible:outline-2 focus-visible:outline-offset-4 focus-visible:outline-[#b94016] disabled:cursor-wait disabled:opacity-60";

export default function OperatorLoginDialog({
  onClose,
  authMode,
  initialError,
}: OperatorLoginDialogProps) {
  const dialogRef = useRef<HTMLDialogElement>(null);
  const initialFocusRef = useRef<HTMLInputElement | HTMLAnchorElement | null>(null);
  const pointerStartedOnBackdrop = useRef(false);
  const requestRef = useRef<AbortController | null>(null);
  const [error, setError] = useState(initialError);
  const [pending, setPending] = useState(false);
  const titleId = useId();
  const descriptionId = useId();
  const errorId = useId();
  const inputId = useId();

  useEffect(() => {
    const dialog = dialogRef.current;
    if (!dialog) return;

    const previousFocus = document.activeElement;
    dialog.showModal();
    initialFocusRef.current?.focus();

    return () => {
      requestRef.current?.abort();
      requestRef.current = null;
      dialog.close();
      if (previousFocus instanceof HTMLElement && previousFocus.isConnected) {
        previousFocus.focus();
      }
    };
  }, []);

  async function signIn(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (requestRef.current) return;
    const form = event.currentTarget;
    const key = new FormData(form).get("key");
    if (typeof key !== "string" || key.length === 0) return;
    const controller = new AbortController();
    requestRef.current = controller;
    setPending(true);
    setError(undefined);

    try {
      const response = await fetch("/api/auth/operator", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ key }),
        signal: controller.signal,
      });
      if (controller.signal.aborted) return;
      if (response.ok) {
        form.reset();
        window.location.assign("/dashboard");
        return;
      }
      setError(
        response.status === 401
          ? "Incorrect access key."
          : "Couldn’t sign in. Please try again.",
      );
    } catch {
      if (!controller.signal.aborted) {
        setError("Couldn’t reach the server. Please try again.");
      }
    } finally {
      if (!controller.signal.aborted) {
        requestRef.current = null;
        setPending(false);
      }
    }
  }

  const operator = authMode === "operator";
  const sso = authMode === "sso";

  return (
    <dialog
      ref={dialogRef}
      aria-labelledby={titleId}
      aria-describedby={descriptionId}
      onCancel={(event) => {
        event.preventDefault();
        onClose();
      }}
      onPointerDown={(event) => {
        pointerStartedOnBackdrop.current = event.target === event.currentTarget;
      }}
      onClick={(event) => {
        if (pointerStartedOnBackdrop.current && event.target === event.currentTarget) {
          onClose();
        }
        pointerStartedOnBackdrop.current = false;
      }}
      className="fixed inset-0 m-auto max-h-[calc(100dvh-2rem)] w-[min(26rem,calc(100%-2rem))] max-w-none overflow-y-auto rounded-2xl bg-[#fff8ed] p-0 text-[#292019] shadow-2xl backdrop:bg-[#211913]/65"
    >
      <div className="relative p-6 sm:p-8">
        <button
          type="button"
          aria-label="Close sign-in"
          onClick={onClose}
          className="absolute right-4 top-4 inline-flex size-10 items-center justify-center rounded-full text-[#695747] hover:bg-[#f1e2d2] focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-[#b94016]"
        >
          <X aria-hidden="true" className="size-5" />
        </button>

        <h2 id={titleId} className="pr-10 text-2xl font-semibold tracking-tight">
          {operator ? "Operator sign-in" : sso ? "Sign in with Agora" : "Open the dashboard"}
        </h2>
        <p id={descriptionId} className="mt-2 text-sm leading-6 text-[#695747]">
          {operator
            ? "Enter your access key to open your agent dashboard."
            : sso
              ? "Use your Agora account to open your agent dashboard."
              : "Continue to your agent dashboard."}
        </p>

        {error && (
          <p id={errorId} role="alert" className="mt-4 text-sm text-[#9c2c14]">
            {error}
          </p>
        )}

        {operator ? (
          <form onSubmit={signIn} className="mt-5 space-y-4">
            <div className="space-y-2">
              <label htmlFor={inputId} className="block text-sm font-semibold">
                Access key
              </label>
              <input
                ref={(input) => { initialFocusRef.current = input; }}
                id={inputId}
                name="key"
                type="password"
                autoComplete="current-password"
                aria-describedby={error ? errorId : undefined}
                required
                className="min-h-11 w-full rounded-lg border border-[#aa9683] bg-white px-3 py-2 text-base focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-[#b94016]"
              />
            </div>
            <button type="submit" disabled={pending} className={actionClass}>
              {pending ? "Signing in…" : "Sign in"}
            </button>
          </form>
        ) : sso ? (
          <a
            ref={(link) => { initialFocusRef.current = link; }}
            href="/api/auth/agora/start"
            className={`mt-5 ${actionClass}`}
          >
            Sign in with Agora
          </a>
        ) : (
          <Link
            ref={(link) => { initialFocusRef.current = link; }}
            href="/dashboard"
            className={`mt-5 ${actionClass}`}
          >
            Open dashboard
          </Link>
        )}
      </div>
    </dialog>
  );
}
