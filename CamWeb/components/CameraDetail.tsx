"use client";

import Link from "next/link";
import { useSearchParams } from "next/navigation";
import { useState } from "react";
import { getBackend } from "@/lib/backend";
import { useServerStatus } from "@/lib/use-server";
import LiveFeed from "./LiveFeed";
import styles from "./CameraDetail.module.css";
import ui from "./ui.module.css";

/** The page of one camera (?name=...): its live video, the data of the camera, and saving its buffer. */
export default function CameraDetail() {
  const name = useSearchParams().get("name") ?? "";
  const { status, error } = useServerStatus();
  const [minutes, setMinutes] = useState(5);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<{ ok: boolean; text: string } | null>(null);

  const camera = status?.cameras.find((c) => c.name === name);

  async function save() {
    setBusy(true);
    try {
      const { files, failed } = await getBackend().saveRecording(name, minutes);
      setMessage({ ok: true, text: `Saved: ${files.join(", ")}${failed.length > 0 ? `. Failed: ${failed.join("; ")}` : ""}` });
    } catch (e) {
      setMessage({ ok: false, text: e instanceof Error ? e.message : String(e) });
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.page}>
      <Link href="/dashboard" className={styles.back}>
        &larr; Overview
      </Link>

      {!name && <p className={`${ui.notice} ${ui.noticeError}`}>No camera was chosen.</p>}
      {error && (
        <p role="alert" className={`${ui.notice} ${ui.noticeError}`}>
          {error}
        </p>
      )}
      {status && name && !camera && <p className={`${ui.notice} ${ui.noticeError}`}>The camera &quot;{name}&quot; is not connected.</p>}

      {name && (
        <>
          <h1 className={styles.title}>{name}</h1>
          <LiveFeed camera={name} online={status ? camera !== undefined && camera.lastSeenMsAgo < 5000 : true} />
        </>
      )}

      {camera && (
        <div className={`${ui.card} ${styles.info}`}>
          <div>
            Address: <span className={ui.mono}>{camera.address}</span>
          </div>
          <div>
            Video: <b>{camera.width && camera.height ? `${camera.width}x${camera.height}` : "size unknown"}</b> at <b>{camera.fps} fps</b>
          </div>
          <div>
            Buffered: <b>{Math.floor(camera.bufferedSeconds / 60)}m {camera.bufferedSeconds % 60}s</b> of {status?.bufferMinutes} min
          </div>
          <div>
            Faces visible: <b>{camera.facesVisible}</b>
          </div>
          <div className={styles.save}>
            <label className={styles.minutes}>
              Save the last
              <input
                type="number"
                min={1}
                max={status?.bufferMinutes}
                value={minutes}
                onChange={(e) => setMinutes(Math.min(Math.max(Number(e.target.value) || 1, 1), status?.bufferMinutes ?? 1))}
                className={styles.minutesInput}
              />
              min
            </label>
            <button className={`${ui.button} ${ui.primary}`} disabled={busy} onClick={save}>
              {busy ? "Saving..." : "Save buffer as video"}
            </button>
          </div>
        </div>
      )}

      {message && (
        <p role="status" className={`${ui.notice} ${message.ok ? ui.noticeOk : ui.noticeError}`}>
          {message.text}
        </p>
      )}
    </div>
  );
}
