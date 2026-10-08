"use client";

import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";
import { startSession, useSession } from "@/lib/auth";
import { getBackend, isMockBackend } from "@/lib/backend";
import { DEMO_PASSWORD, DEMO_USERNAME } from "@/lib/mock-backend";
import styles from "./LoginForm.module.css";
import ui from "./ui.module.css";

export default function LoginForm() {
  const router = useRouter();
  const { user } = useSession();
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  // Whoever is logged in already does not need the login screen.
  useEffect(() => {
    if (user) {
      router.replace("/dashboard");
    }
  }, [user, router]);

  async function submit(event: React.FormEvent) {
    event.preventDefault();
    setBusy(true);
    setError(null);
    try {
      startSession(await getBackend().login(username, password));
      router.replace("/dashboard");
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.page}>
      <form onSubmit={submit} className={`${ui.card} ${styles.form}`}>
        <div>
          <h1 className={styles.title}>CamVision</h1>
          <p className={`${styles.subtitle} ${ui.muted}`}>Log in to control your cameras and displays.</p>
        </div>

        <label className={styles.field}>
          User name
          <input className={ui.input} value={username} onChange={(e) => setUsername(e.target.value)} autoComplete="username" autoFocus required />
        </label>

        <label className={styles.field}>
          Password
          <input
            className={ui.input}
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoComplete="current-password"
            required
          />
        </label>

        {error && (
          <p role="alert" className={`${ui.notice} ${ui.noticeError}`}>
            {error}
          </p>
        )}

        <button type="submit" disabled={busy} className={`${ui.button} ${ui.primary} ${styles.submit}`}>
          {busy ? "Logging in..." : "Log in"}
        </button>

        {isMockBackend && (
          <p className={`${ui.notice} ${ui.noticeWarn}`}>
            Demo mode (no backend): <b>{DEMO_USERNAME}</b> / <b>{DEMO_PASSWORD}</b>
          </p>
        )}
      </form>
    </div>
  );
}
