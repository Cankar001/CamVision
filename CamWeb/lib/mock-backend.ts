import type { CamBackend } from "./backend";
import type { ConnectionInput, NewDevice, Recording, RecordingList, SaveResult, ServerConnection, ServerStatus, StreamLink } from "./types";

const delay = (ms = 150) => new Promise((resolve) => setTimeout(resolve, ms));

/** The login of the demo (NEXT_PUBLIC_BACKEND=mock). It is checked in the browser, which is NOT secure: it only exists to work on the screens. */
export const DEMO_USERNAME = "admin";
export const DEMO_PASSWORD = "camvision";

/** Demo data, which behaves a little like the real server: the buffers grow, saving makes recordings, stopping ends it. */
export class MockBackend implements CamBackend {
  private started = Date.now();
  private stopped = false;
  private removed = new Set<string>();
  private added: { name: string; role: "camera" | "display"; keyId: string }[] = [];
  private connection: ServerConnection = { host: "127.0.0.1", port: 45651, secure: false, tokenSet: true, tokenSource: "file", state: "connected", error: null };
  private recordings: Recording[] = [
    { file: "Front_door/20261008_170211_web.avi", camera: "Front_door", sizeMegabytes: 38.2, time: "2026-10-08T17:02:11.000Z" },
    { file: "Garage/20261008_164055_web.avi", camera: "Garage", sizeMegabytes: 21.7, time: "2026-10-08T16:40:55.000Z" },
  ];

  private cameras = [
    { name: "Front door", address: "192.168.1.21:51234", fps: 30, width: 1280, height: 720, facesVisible: 1 },
    { name: "Garage", address: "192.168.1.22:50871", fps: 15, width: 1280, height: 720, facesVisible: 0 },
    { name: "Garden", address: "192.168.1.23:49902", fps: 25, width: 1920, height: 1080, facesVisible: 0 },
  ];

  private displays = [
    { name: "Living room", address: "192.168.1.40:50011", camera: "", maxFps: 30 },
    { name: "Hallway", address: "192.168.1.41:50342", camera: "Front door", maxFps: 15 },
  ];

  private ensureRunning() {
    if (this.stopped) {
      throw new Error("The server is not reachable (the demo server was stopped, reload the page to start it again).");
    }
  }

  async login(username: string, password: string): Promise<string> {
    await delay();
    if (username.trim().toLowerCase() === DEMO_USERNAME && password === DEMO_PASSWORD) {
      return DEMO_USERNAME;
    }

    throw new Error("Wrong user name or password.");
  }

  async logout(): Promise<void> {}

  async getStatus(): Promise<ServerStatus> {
    await delay();
    this.ensureRunning();
    const uptimeSeconds = Math.floor((Date.now() - this.started) / 1000);
    const bufferMinutes = 5;

    const cameras = this.cameras
      .filter((c) => !this.removed.has(c.name))
      .map((camera) => ({ ...camera, bufferedSeconds: Math.min(uptimeSeconds + 40, bufferMinutes * 60), lastSeenMsAgo: 30 }));

    const devices = [
      ...this.cameras.map((c, i) => ({ name: c.name, role: "camera" as const, keyId: `c0a${i}f2`, online: true })),
      ...this.displays.map((d, i) => ({ name: d.name, role: "display" as const, keyId: `d1b${i}e7`, online: true })),
      { name: "Basement", role: "camera" as const, keyId: "c0a9d4", online: false },
      ...this.added.map((d) => ({ ...d, online: false })),
    ].filter((device) => !this.removed.has(device.name));

    return {
      cameras,
      displays: this.displays.filter((d) => !this.removed.has(d.name)),
      devices,
      bufferMinutes,
      deviceKeysRequired: true,
      recording: { folder: "recordings", scheduled: true, schedule: "22:00-06:00", defaultMinutes: 5 },
    };
  }

  async saveRecording(camera: string, minutes: number): Promise<SaveResult> {
    await delay(500);
    this.ensureRunning();
    const targets = this.cameras.filter((c) => (!camera || c.name === camera) && !this.removed.has(c.name));
    if (targets.length === 0) {
      throw new Error(camera ? `There is no camera "${camera}".` : "No camera is connected.");
    }

    const stamp = new Date().toISOString();
    const files = targets.map((c) => {
      const folder = c.name.replace(/\s+/g, "_");
      const file = `${folder}/${stamp.slice(0, 19).replace(/[-:]/g, "").replace("T", "_")}_web.avi`;
      this.recordings.unshift({ file, camera: folder, sizeMegabytes: Math.round(minutes * 74) / 10, time: stamp });
      return file;
    });

    return { files, failed: [] };
  }

  async listRecordings(limit: number): Promise<RecordingList> {
    await delay();
    this.ensureRunning();
    return { recordings: this.recordings.slice(0, limit), total: this.recordings.length, downloadable: false };
  }

  recordingDownloadUrl(file: string): string {
    return `#${file}`;
  }

  async requestStream(camera: string): Promise<StreamLink> {
    await delay(400);
    this.ensureRunning();
    if (!this.cameras.some((c) => c.name === camera && !this.removed.has(c.name))) {
      throw new Error(`There is no camera "${camera}".`);
    }

    // Demo stream: an animated picture as a data URL (the real backend answers with the URL of its MJPEG stream).
    const hue = (camera.length * 47) % 360;
    const label = camera.replace(/[<>&"]/g, "");
    const svg =
      `<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360" viewBox="0 0 640 360">` +
      `<rect width="640" height="360" fill="hsl(${hue} 35% 18%)"/>` +
      `<circle cy="180" r="36" fill="hsl(${hue} 60% 55%)"><animate attributeName="cx" values="100;540;100" dur="6s" repeatCount="indefinite"/>` +
      `<animate attributeName="cy" values="120;240;120" dur="4s" repeatCount="indefinite"/></circle>` +
      `<text x="16" y="32" fill="#fff" font-family="sans-serif" font-size="20">${label} (demo stream)</text></svg>`;
    return { url: `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`, format: "mjpeg" };
  }

  async addDevice(role: "camera" | "display", name: string): Promise<NewDevice> {
    await delay();
    this.ensureRunning();
    if (!name.trim()) {
      throw new Error("The device needs a name.");
    }

    const keyId = Math.random().toString(16).slice(2, 10);
    this.added.push({ name, role, keyId });
    return { name, role, keyId, key: Array.from({ length: 64 }, () => Math.floor(Math.random() * 16).toString(16)).join("") };
  }

  async removeDevice(name: string): Promise<void> {
    await delay();
    this.ensureRunning();
    this.removed.add(name);
  }

  async stopServer(): Promise<void> {
    await delay(300);
    this.stopped = true;
  }

  async getConnection(): Promise<ServerConnection> {
    await delay();
    return this.connection;
  }

  async saveConnection(input: ConnectionInput): Promise<ServerConnection> {
    await delay();
    this.connection = {
      ...this.connection,
      host: input.host,
      port: input.port,
      secure: input.secure,
      tokenSet: this.connection.tokenSet || input.token !== "",
      tokenSource: input.token ? "settings" : this.connection.tokenSource,
    };
    return this.connection;
  }

  async testConnection(input: ConnectionInput): Promise<{ ok: boolean; error: string | null }> {
    await delay(500);
    return input.token === "wrong" ? { ok: false, error: "CamServer did not accept the token (wrong token)." } : { ok: true, error: null };
  }
}
