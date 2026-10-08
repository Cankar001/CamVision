"use client";

import { useEffect, useState } from "react";
import { getBackend } from "@/lib/backend";
import type { RecordingList } from "@/lib/types";
import styles from "./Recordings.module.css";
import ui from "./ui.module.css";

const LIMIT = 100;

export default function Recordings() {
  const [list, setList] = useState<RecordingList | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    getBackend()
      .listRecordings(LIMIT)
      .then((result) => active && setList(result))
      .catch((e) => active && setError(e instanceof Error ? e.message : String(e)));
    return () => {
      active = false;
    };
  }, []);

  return (
    <div className={styles.page}>
      <div>
        <h1 className={styles.title}>Recordings</h1>
        <p className={`${styles.subtitle} ${ui.muted}`}>
          {list ? `The newest ${list.recordings.length} of ${list.total} videos.` : "The videos on the server, newest first."}
          {list && !list.downloadable && " Downloads are off: set RECORDINGS_DIR in the backend."}
        </p>
      </div>

      {error && <p className={`${ui.notice} ${ui.noticeError}`}>{error}</p>}
      {!list && !error && <p className={ui.muted}>Loading...</p>}
      {list && list.recordings.length === 0 && <p className={ui.muted}>There are no recordings yet.</p>}

      {list && list.recordings.length > 0 && (
        <div className={`${ui.card} ${styles.table}`}>
          {list.recordings.map((recording) => (
            <div key={recording.file} className={styles.row}>
              <span className={styles.camera}>{recording.camera || "-"}</span>
              <span className={`${styles.file} ${ui.mono}`}>{recording.file}</span>
              <span className={ui.muted}>{recording.sizeMegabytes} MB</span>
              <time className={ui.muted} dateTime={recording.time}>
                {new Date(recording.time).toLocaleString()}
              </time>
              {list.downloadable && (
                <a className={`${ui.button} ${ui.secondary}`} href={getBackend().recordingDownloadUrl(recording.file)} download>
                  Download
                </a>
              )}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
