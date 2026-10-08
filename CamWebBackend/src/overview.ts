import type { CamServerApi, RawStatus } from "./camserver/api.ts";
import { jpegSize } from "./jpeg.ts";
import type { Camera, Device, Display, Recording, ServerStatus } from "./types.ts";

// A camera, whose size could not be found out, is not asked again for this long.
const SIZE_RETRY_MS = 30000;

/** Puts the answers of several CamServer commands together to the overview of the web UI. */
export class Overview {
  private readonly sizes = new Map<string, { width: number; height: number }>();
  private readonly failedAt = new Map<string, number>();

  constructor(private readonly api: CamServerApi) {}

  /** The size of the pictures of a camera is only in the pictures themselves: the live view and the overview note it. */
  rememberPicture(camera: string, jpeg: Buffer) {
    const size = jpegSize(jpeg);
    if (size) {
      this.sizes.set(camera, size);
    }
  }

  async get(): Promise<ServerStatus> {
    const [status, displays, devices] = await Promise.all([this.api.status(), this.api.displays(), this.api.devices()]);
    await this.findSizes(status);

    const cameras: Camera[] = status.cameras.map((camera) => ({
      name: camera.name,
      address: camera.address,
      // The server does not know the frame rate: it is the number of buffered frames over the time they span.
      fps: camera.seconds_buffered > 0 ? Math.round(camera.frames_buffered / camera.seconds_buffered) : 0,
      width: this.sizes.get(camera.name)?.width ?? null,
      height: this.sizes.get(camera.name)?.height ?? null,
      bufferedSeconds: Math.round(camera.seconds_buffered),
      lastSeenMsAgo: camera.last_seen_ms_ago,
      facesVisible: camera.faces_visible,
    }));

    const connectedDisplays: Display[] = displays.map((display) => ({
      name: display.name,
      address: display.address,
      camera: display.camera,
      maxFps: display.max_fps,
    }));

    // A device is online, if a camera or display of that name is connected.
    const online = (role: Device["role"], name: string) => (role === "camera" ? cameras : connectedDisplays).some((entry) => entry.name === name);
    const allDevices: Device[] = devices.map((device) => ({
      name: device.name,
      role: device.role,
      keyId: device.key_id,
      online: online(device.role, device.name),
    }));

    return {
      cameras,
      displays: connectedDisplays,
      devices: allDevices,
      bufferMinutes: status.buffer_minutes,
      deviceKeysRequired: status.device_keys_required,
      recording: {
        folder: status.recording.folder,
        scheduled: status.recording.scheduled,
        schedule: status.recording.schedule,
        defaultMinutes: status.recording.default_minutes,
      },
    };
  }

  private async findSizes(status: RawStatus) {
    const now = Date.now();
    const missing = status.cameras.filter((camera) => !this.sizes.has(camera.name) && now - (this.failedAt.get(camera.name) ?? 0) > SIZE_RETRY_MS);
    await Promise.all(
      missing.map(async (camera) => {
        try {
          const snapshot = await this.api.snapshot(camera.name, 0);
          if (snapshot.jpeg) {
            this.rememberPicture(camera.name, Buffer.from(snapshot.jpeg, "base64"));
          }
        } catch {
          this.failedAt.set(camera.name, now);
        }
      }),
    );
  }
}

/** The recordings folder has one sub folder per camera. */
export function toRecording(entry: { file: string; bytes: number; modified: number }): Recording {
  const separator = entry.file.indexOf("/");
  return {
    file: entry.file,
    camera: separator > 0 ? entry.file.slice(0, separator) : "",
    sizeMegabytes: Math.round((entry.bytes / (1024 * 1024)) * 10) / 10,
    time: new Date(entry.modified * 1000).toISOString(),
  };
}
