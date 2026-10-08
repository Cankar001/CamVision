"use client";

import { useEffect, useRef, useState } from "react";
import { getBackend } from "@/lib/backend";
import type { StreamLink } from "@/lib/types";
import styles from "./LiveFeed.module.css";
import ui from "./ui.module.css";

type Feed =
  | { phase: "requesting" }
  | { phase: "ready"; link: StreamLink; playing: boolean }
  | { phase: "error"; message: string };

/**
 * The live video of one camera. When it is shown, it asks the backend for a stream link, and the backend answers with the URL of the live stream, which is
 * shown here (and in the text below the picture).
 */
export default function LiveFeed({ camera, online = true }: { camera: string; online?: boolean }) {
  const frameRef = useRef<HTMLDivElement>(null);
  const [feed, setFeed] = useState<Feed>({ phase: "requesting" });
  const [attempt, setAttempt] = useState(0);
  const [wasOnline, setWasOnline] = useState(online);

  // The stream of a camera, which stopped sending, just ends (the picture stays): when the camera is back, the old link is useless, so a new one is requested.
  if (online !== wasOnline) {
    setWasOnline(online);
    if (online) {
      setFeed({ phase: "requesting" });
      setAttempt((n) => n + 1);
    }
  }

  useEffect(() => {
    let active = true;
    getBackend()
      .requestStream(camera)
      .then((link) => active && setFeed({ phase: "ready", link, playing: false }))
      .catch((e) => active && setFeed({ phase: "error", message: e instanceof Error ? e.message : String(e) }));

    return () => {
      active = false;
    };
  }, [camera, attempt]);

  // Asks for a new link: for the first time, or if the stream ended (links can run out).
  function reconnect() {
    setFeed({ phase: "requesting" });
    setAttempt((n) => n + 1);
  }

  function toggleFullscreen() {
    if (document.fullscreenElement) {
      document.exitFullscreen();
    } else {
      frameRef.current?.requestFullscreen();
    }
  }

  const markPlaying = () => setFeed((f) => (f.phase === "ready" ? { ...f, playing: true } : f));
  const streamEnded = () => setFeed({ phase: "error", message: "The stream ended or could not be loaded." });

  const playing = feed.phase === "ready" && feed.playing && online;

  let overlay: string | null = null;
  if (!online) {
    overlay = "The camera is not sending pictures. The video continues when it is back.";
  } else if (feed.phase === "requesting") {
    overlay = "Requesting the stream...";
  } else if (feed.phase === "error") {
    overlay = feed.message;
  } else if (!feed.playing) {
    overlay = "Waiting for the video...";
  }

  return (
    <div className={styles.wrapper}>
      <div ref={frameRef} className={styles.frame}>
        {feed.phase === "ready" &&
          (feed.link.format === "video" ? (
            <video className={styles.media} src={feed.link.url} autoPlay muted playsInline onPlaying={markPlaying} onError={streamEnded} />
          ) : (
            // eslint-disable-next-line @next/next/no-img-element -- a live stream, which the image optimization cannot handle
            <img className={styles.media} src={feed.link.url} alt={`Live video of ${camera}`} onLoad={markPlaying} onError={streamEnded} />
          ))}
        {overlay && <div className={styles.overlay}>{overlay}</div>}
        <div className={styles.status}>
          <span className={`${ui.badge} ${playing ? ui.badgeOn : ""}`}>
            <span className={ui.dot} />
            {playing ? "Live" : !online ? "No signal" : feed.phase === "error" ? "Not available" : "Connecting..."}
          </span>
        </div>
      </div>

      <div className={styles.buttons}>
        <button className={`${ui.button} ${ui.secondary}`} onClick={toggleFullscreen} disabled={feed.phase !== "ready"}>
          Fullscreen
        </button>
        <button className={`${ui.button} ${ui.secondary}`} onClick={reconnect} disabled={feed.phase === "requesting"}>
          {feed.phase === "error" ? "Try again" : "Request a new link"}
        </button>
      </div>

      {feed.phase === "ready" && (
        <p className={styles.url}>
          Stream URL ({feed.link.format}): <span className={ui.mono}>{feed.link.url.length > 120 ? `${feed.link.url.slice(0, 120)}...` : feed.link.url}</span>
        </p>
      )}
    </div>
  );
}
