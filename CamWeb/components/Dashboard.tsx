"use client";

import Link from "next/link";
import { useState } from "react";
import { getBackend } from "@/lib/backend";
import { useServerStatus } from "@/lib/use-server";
import type { Camera, Device, Display, NewDevice } from "@/lib/types";
import styles from "./Dashboard.module.css";
import ui from "./ui.module.css";

function formatDuration(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  return h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${s}s` : `${s}s`;
}

const primary = `${ui.button} ${ui.primary}`;
const secondary = `${ui.button} ${ui.secondary}`;
const danger = `${ui.button} ${ui.danger}`;

function Badge({ on, onText, offText }: { on: boolean; onText: string; offText: string }) {
  return (
    <span className={`${ui.badge} ${on ? ui.badgeOn : ""}`}>
      <span className={ui.dot} />
      {on ? onText : offText}
    </span>
  );
}

export default function Dashboard() {
  const { status, error, refresh } = useServerStatus();
  const [minutes, setMinutes] = useState(5);
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<{ ok: boolean; text: string } | null>(null);
  const [confirm, setConfirm] = useState<string | null>(null);
  const [newDevice, setNewDevice] = useState<NewDevice | null>(null);
  const [deviceRole, setDeviceRole] = useState<"camera" | "display">("camera");
  const [deviceName, setDeviceName] = useState("");

  async function run(key: string, action: () => Promise<string>) {
    setBusy(key);
    setConfirm(null);
    try {
      setMessage({ ok: true, text: await action() });
    } catch (e) {
      setMessage({ ok: false, text: e instanceof Error ? e.message : String(e) });
    } finally {
      setBusy(null);
      refresh();
    }
  }

  const save = (camera: string) =>
    run(`save:${camera}`, async () => {
      const { files, failed } = await getBackend().saveRecording(camera, minutes);
      const failures = failed.length > 0 ? ` Failed: ${failed.join("; ")}` : "";
      return `Saved ${files.length} video${files.length === 1 ? "" : "s"}: ${files.join(", ")}.${failures}`;
    });

  const removeDevice = (device: Device) =>
    run(`remove:${device.name}`, async () => {
      await getBackend().removeDevice(device.name);
      return `Removed the ${device.role} "${device.name}". Its key does not work anymore.`;
    });

  const addDevice = (event: React.FormEvent) => {
    event.preventDefault();
    return run("add", async () => {
      const device = await getBackend().addDevice(deviceRole, deviceName.trim());
      setNewDevice(device);
      setDeviceName("");
      return `Added the ${device.role} "${device.name}".`;
    });
  };

  const stopServer = () =>
    run("stop", async () => {
      await getBackend().stopServer();
      return "The server was told to stop.";
    });

  if (!status) {
    return <p className={error ? `${ui.notice} ${ui.noticeError}` : ui.muted}>{error ?? "Connecting to the server..."}</p>;
  }

  return (
    <div className={styles.page}>
      <div className={styles.top}>
        <div>
          <h1 className={styles.title}>Overview</h1>
          <p className={`${styles.subtitle} ${ui.muted}`}>
            {status.cameras.length} camera{status.cameras.length === 1 ? "" : "s"}, {status.displays.length} display
            {status.displays.length === 1 ? "" : "s"} connected.
          </p>
        </div>

        <div className={styles.actions}>
          <label className={styles.minutes}>
            Save the last
            <input
              type="number"
              min={1}
              max={status.bufferMinutes}
              value={minutes}
              onChange={(e) => setMinutes(Math.min(Math.max(Number(e.target.value) || 1, 1), status.bufferMinutes))}
              className={styles.minutesInput}
            />
            min
          </label>
          <button className={primary} disabled={busy !== null || status.cameras.length === 0} onClick={() => save("")}>
            {busy === "save:" ? "Saving..." : "Save all cameras"}
          </button>
          {confirm === "stop" ? (
            <>
              <button className={danger} disabled={busy !== null} onClick={stopServer}>
                Really stop the server
              </button>
              <button className={secondary} onClick={() => setConfirm(null)}>
                Cancel
              </button>
            </>
          ) : (
            <button className={secondary} disabled={busy !== null} onClick={() => setConfirm("stop")}>
              Stop server
            </button>
          )}
        </div>
      </div>

      {error && (
        <p role="alert" className={`${ui.notice} ${ui.noticeError}`}>
          {error}
        </p>
      )}
      {message && (
        <p role="status" className={`${ui.notice} ${message.ok ? ui.noticeOk : ui.noticeError}`}>
          {message.text}
        </p>
      )}

      <section className={styles.section}>
        <h2 className={styles.sectionTitle}>Cameras</h2>
        {status.cameras.length === 0 ? (
          <p className={styles.empty}>No camera is connected.</p>
        ) : (
          <div className={styles.grid}>
            {status.cameras.map((camera) => (
              <CameraCard key={camera.name} camera={camera} bufferMinutes={status.bufferMinutes} busy={busy} onSave={() => save(camera.name)} />
            ))}
          </div>
        )}
      </section>

      <section className={styles.section}>
        <h2 className={styles.sectionTitle}>Displays</h2>
        {status.displays.length === 0 ? (
          <p className={styles.empty}>No display is connected.</p>
        ) : (
          <div className={styles.grid}>
            {status.displays.map((display) => (
              <DisplayCard key={display.name} display={display} />
            ))}
          </div>
        )}
      </section>

      <section className={styles.section}>
        <h2 className={styles.sectionTitle}>Devices with a key</h2>
        <div className={`${ui.card} ${styles.devices}`}>
          {status.devices.length === 0 && <p className={styles.deviceEmpty}>There are no devices yet.</p>}
          {status.devices.map((device) => (
            <div key={device.name} className={styles.deviceRow}>
              <span className={styles.deviceName}>{device.name}</span>
              <span className={ui.muted}>{device.role}</span>
              <span className={`${styles.keyId} ${ui.mono}`}>key id {device.keyId}</span>
              <Badge on={device.online} onText="online" offText="offline" />
              <div className={styles.deviceActions}>
                {confirm === `remove:${device.name}` ? (
                  <>
                    <button className={danger} disabled={busy !== null} onClick={() => removeDevice(device)}>
                      Remove, its key stops working
                    </button>
                    <button className={secondary} onClick={() => setConfirm(null)}>
                      Cancel
                    </button>
                  </>
                ) : (
                  <button className={secondary} disabled={busy !== null} onClick={() => setConfirm(`remove:${device.name}`)}>
                    Remove
                  </button>
                )}
              </div>
            </div>
          ))}
        </div>
      </section>

      <section className={styles.section}>
        <h2 className={styles.sectionTitle}>Add a device</h2>
        <form onSubmit={addDevice} className={`${ui.card} ${styles.addDevice}`}>
          <select className={ui.input} value={deviceRole} onChange={(e) => setDeviceRole(e.target.value as "camera" | "display")} aria-label="Kind of device">
            <option value="camera">Camera</option>
            <option value="display">Display</option>
          </select>
          <input className={ui.input} value={deviceName} onChange={(e) => setDeviceName(e.target.value)} placeholder="Name, e.g. Front door" maxLength={64} required />
          <button type="submit" className={primary} disabled={busy !== null || !deviceName.trim()}>
            {busy === "add" ? "Adding..." : "Add device"}
          </button>
        </form>
        {newDevice && (
          <div className={`${ui.notice} ${ui.noticeWarn} ${styles.newKey}`}>
            <p>
              Put these lines into <span className={ui.mono}>{newDevice.role === "camera" ? "client.cfg" : "display.cfg"}</span> on the device. The key is shown
              only now and is not kept by the web UI.
            </p>
            <pre className={ui.mono}>
              key = {newDevice.key}
              {newDevice.role === "camera" ? `
name = ${newDevice.name}` : ""}
            </pre>
            <button className={secondary} onClick={() => setNewDevice(null)}>
              I have copied it
            </button>
          </div>
        )}
      </section>

      <section className={styles.section}>
        <h2 className={styles.sectionTitle}>Recording</h2>
        <div className={`${ui.card} ${styles.facts}`}>
          <div>
            Buffer: <b>{status.bufferMinutes} min</b> per camera
          </div>
          <div>
            Folder: <span className={ui.mono}>{status.recording.folder}</span>
          </div>
          <div>
            Schedule: <b>{status.recording.scheduled ? status.recording.schedule : "off"}</b>
          </div>
          <div>
            Device keys required: <b>{status.deviceKeysRequired ? "yes" : "no"}</b>
          </div>
        </div>
      </section>
    </div>
  );
}

function CameraCard({ camera, bufferMinutes, busy, onSave }: { camera: Camera; bufferMinutes: number; busy: string | null; onSave: () => void }) {
  const percent = Math.min(100, Math.round((camera.bufferedSeconds / (bufferMinutes * 60)) * 100));
  return (
    <div className={`${ui.card} ${styles.tile} ${styles.clickable}`}>
      <div className={styles.tileHead}>
        <div>
          <h3 className={styles.tileName}>
            <Link href={`/dashboard/camera?name=${encodeURIComponent(camera.name)}`} className={styles.tileLink}>
              {camera.name}
            </Link>
          </h3>
          <p className={`${styles.address} ${ui.mono}`}>{camera.address}</p>
        </div>
        <Badge on={camera.lastSeenMsAgo < 5000} onText="online" offText="no signal" />
      </div>
      <p className={styles.info}>
        {camera.width && camera.height ? `${camera.width}x${camera.height}` : "size unknown"} at {camera.fps} fps
        {camera.facesVisible > 0 && `, ${camera.facesVisible} face${camera.facesVisible === 1 ? "" : "s"} visible`}
      </p>
      <div>
        <div className={styles.bufferHead}>
          <span>Buffer</span>
          <span>{formatDuration(camera.bufferedSeconds)}</span>
        </div>
        <div className={styles.track}>
          <div className={styles.bar} style={{ width: `${percent}%` }} />
        </div>
      </div>
      <button className={`${secondary} ${styles.tileButton}`} disabled={busy !== null} onClick={onSave}>
        {busy === `save:${camera.name}` ? "Saving..." : "Save buffer as video"}
      </button>
    </div>
  );
}

function DisplayCard({ display }: { display: Display }) {
  return (
    <div className={`${ui.card} ${styles.tile}`}>
      <div>
        <h3 className={styles.tileName}>{display.name}</h3>
        <p className={`${styles.address} ${ui.mono}`}>{display.address}</p>
      </div>
      <p className={styles.info}>
        Shows: {display.camera || "all cameras"} (max {display.maxFps} fps)
      </p>
    </div>
  );
}
