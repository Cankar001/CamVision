"use client";

import { useEffect, useState } from "react";
import { getBackend } from "@/lib/backend";
import { DEFAULT_PREFERENCES, savePreferences, usePreferences } from "@/lib/preferences";
import type { ServerConnection } from "@/lib/types";
import styles from "./ConnectionForm.module.css";
import ui from "./ui.module.css";

interface Draft {
  host: string;
  port: string;
  secure: boolean;
  token: string;
}

const isLocalHost = (host: string) => ["localhost", "127.0.0.1", "::1", "[::1]"].includes(host.trim().toLowerCase());

const STATE_TEXT: Record<ServerConnection["state"], string> = {
  idle: "Not connected yet (it connects with the next request)",
  connecting: "Connecting...",
  connected: "Connected",
  error: "Not connected",
};

/**
 * The connection of the backend to the remote control (WebSocket) of CamServer: the websocket_* settings of its server.cfg. They are kept by the backend,
 * the token is only ever sent to it, never shown again.
 */
export default function ConnectionForm() {
  const preferences = usePreferences();
  const [current, setCurrent] = useState<ServerConnection | null>(null);
  const [draft, setDraft] = useState<Draft | null>(null);
  const [showToken, setShowToken] = useState(false);
  const [busy, setBusy] = useState<"save" | "test" | null>(null);
  const [message, setMessage] = useState<{ ok: boolean; text: string } | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    getBackend()
      .getConnection()
      .then((connection) => active && setCurrent(connection))
      .catch((e) => active && setLoadError(e instanceof Error ? e.message : String(e)));
    return () => {
      active = false;
    };
  }, []);

  if (!current) {
    return <p className={loadError ? `${ui.notice} ${ui.noticeError}` : ui.muted}>{loadError ?? "Loading..."}</p>;
  }

  const values: Draft = draft ?? { host: current.host, port: String(current.port), secure: current.secure, token: "" };
  const port = Number(values.port);
  const host = values.host.trim();
  const sameServer = host.toLowerCase() === current.host.toLowerCase() && port === current.port && values.secure === current.secure;

  const errors: Partial<Record<keyof Draft, string>> = {};
  if (!host || /[\s/]/.test(host)) {
    errors.host = "Enter a host name or an IP address (without ws:// or a path).";
  }

  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    errors.port = "The port is a number between 1 and 65535.";
  }

  // The saved token is only used for the server it was made for.
  if (!values.token.trim() && (!sameServer || !current.tokenSet)) {
    errors.token = sameServer ? "There is no token yet." : "For another server, enter its token too.";
  }

  const hasErrors = Object.keys(errors).length > 0;
  const insecure = !values.secure && !isLocalHost(host);
  const input = { host, port, secure: values.secure, token: values.token.trim() };

  const update = (patch: Partial<Draft>) => {
    setDraft({ ...values, ...patch });
    setMessage(null);
  };

  async function run(kind: "save" | "test") {
    setBusy(kind);
    setMessage(null);
    try {
      if (kind === "test") {
        const result = await getBackend().testConnection(input);
        setMessage(result.ok ? { ok: true, text: "CamServer answered. The settings work." } : { ok: false, text: result.error ?? "No answer." });
      } else {
        setCurrent(await getBackend().saveConnection(input));
        setDraft(null);
        setMessage({ ok: true, text: "Saved. The backend uses the new settings from now on." });
      }
    } catch (e) {
      setMessage({ ok: false, text: e instanceof Error ? e.message : String(e) });
    } finally {
      setBusy(null);
    }
  }

  const error = (field: keyof Draft) => (errors[field] ? <span className={styles.error}>{errors[field]}</span> : null);
  const tokenSource = { settings: "saved here", env: "from the environment of the backend", file: "from the token file of CamServer", none: "none" }[current.tokenSource];

  return (
    <div className={styles.page}>
      <div>
        <h1 className={styles.title}>Connection</h1>
        <p className={`${styles.subtitle} ${ui.muted}`}>
          How the backend reaches the remote control of the CamServer (<span className={ui.mono}>websocket_*</span> in server.cfg).
        </p>
      </div>

      <p className={`${ui.notice} ${current.state === "connected" ? ui.noticeOk : current.state === "error" ? ui.noticeError : ui.noticeWarn}`}>
        <b>{STATE_TEXT[current.state]}.</b> {current.error}
      </p>

      <form
        onSubmit={(event) => {
          event.preventDefault();
          if (!hasErrors) {
            void run("save");
          }
        }}
        className={`${ui.card} ${styles.form}`}
      >
        <div className={styles.row}>
          <label className={styles.field}>
            Host
            <input className={ui.input} value={values.host} onChange={(e) => update({ host: e.target.value })} spellCheck={false} />
            {error("host")}
          </label>
          <label className={styles.field}>
            Port
            <input className={ui.input} type="number" value={values.port} onChange={(e) => update({ port: e.target.value })} />
            {error("port")}
          </label>
        </div>

        <label className={styles.check}>
          <input type="checkbox" checked={values.secure} onChange={(e) => update({ secure: e.target.checked })} />
          Secure connection (wss://)
        </label>
        {insecure && (
          <p className={`${ui.notice} ${ui.noticeWarn}`}>
            This connection goes through the network without encryption: the token and all answers can be read by others. Use it only in a network you trust,
            or through an SSH tunnel or a TLS proxy (then turn on the secure connection).
          </p>
        )}

        <label className={styles.field}>
          Token
          <div className={styles.tokenRow}>
            <input
              className={`${ui.input} ${ui.mono}`}
              type={showToken ? "text" : "password"}
              value={values.token}
              onChange={(e) => update({ token: e.target.value })}
              placeholder={current.tokenSet ? "unchanged (the token is kept secret)" : ""}
              autoComplete="off"
              spellCheck={false}
            />
            <button type="button" className={`${ui.button} ${ui.secondary}`} onClick={() => setShowToken(!showToken)}>
              {showToken ? "Hide" : "Show"}
            </button>
          </div>
          <span className={styles.hint}>
            Token: {tokenSource}. Print it on the server computer with <span className={ui.mono}>CamServer --show_websocket_token</span>.
          </span>
          {error("token")}
        </label>

        <label className={styles.field}>
          Refresh the overview every (seconds, only in this browser)
          <input
            className={`${ui.input} ${styles.short}`}
            type="number"
            min={1}
            max={60}
            value={preferences.refreshSeconds}
            onChange={(e) => savePreferences({ refreshSeconds: Math.min(Math.max(Number(e.target.value) || DEFAULT_PREFERENCES.refreshSeconds, 1), 60) })}
          />
        </label>

        <p className={styles.address}>
          Address: <span className={`${styles.addressValue} ${ui.mono}`}>{`${values.secure ? "wss" : "ws"}://${host}:${values.port}`}</span>
        </p>

        {message && (
          <p role="status" className={`${ui.notice} ${message.ok ? ui.noticeOk : ui.noticeError}`}>
            {message.text}
          </p>
        )}

        <div className={styles.buttons}>
          <button type="submit" disabled={hasErrors || busy !== null} className={`${ui.button} ${ui.primary}`}>
            {busy === "save" ? "Saving..." : "Save"}
          </button>
          <button type="button" disabled={hasErrors || busy !== null} className={`${ui.button} ${ui.secondary}`} onClick={() => run("test")}>
            {busy === "test" ? "Testing..." : "Test connection"}
          </button>
        </div>
      </form>

      <p className={styles.footnote}>These settings are kept by the backend. The refresh interval is only a setting of this browser.</p>
    </div>
  );
}
