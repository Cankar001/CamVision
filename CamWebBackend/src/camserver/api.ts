import type { CamServerClient } from "./client.ts";

// What CamServer answers (see "Remote control" in the README of the repository).

export interface RawCamera {
  name: string;
  address: string;
  frames_buffered: number;
  seconds_buffered: number;
  last_seen_ms_ago: number;
  faces_visible: number;
}

export interface RawStatus {
  cameras: RawCamera[];
  displays: number;
  buffer_minutes: number;
  device_keys_required: boolean;
  recording: { folder: string; scheduled: boolean; schedule: string; default_minutes: number };
}

export interface RawDisplay {
  name: string;
  address: string;
  camera: string;
  max_fps: number;
}

export interface RawDevice {
  name: string;
  role: "camera" | "display";
  key_id: string;
}

export interface RawClip {
  camera: string;
  ok: boolean;
  frames: number;
  file?: string;
  error?: string;
}

export interface RawRecording {
  file: string;
  bytes: number;
  /** Seconds since 1970. */
  modified: number;
}

export interface RawSnapshot {
  camera: string;
  frame: number;
  unchanged?: boolean;
  /** Base64. */
  jpeg?: string;
}

/**
 * The commands of CamServer as functions. The control connection is for everything the user does, the stream connection only for the pictures of the
 * live view (a long command on the first must not freeze the video).
 */
export class CamServerApi {
  constructor(
    readonly control: CamServerClient,
    readonly stream: CamServerClient,
  ) {}

  status() {
    return this.control.request<RawStatus>("status");
  }

  async displays() {
    return (await this.control.request<{ displays: RawDisplay[] }>("displays")).displays;
  }

  /** The devices with a key. Empty, if CamServer runs without device keys (auth = false). */
  async devices(): Promise<RawDevice[]> {
    try {
      return (await this.control.request<{ devices: RawDevice[] }>("devices")).devices;
    } catch (error) {
      if (error instanceof Error && /device keys are turned off/.test(error.message)) {
        return [];
      }

      throw error;
    }
  }

  save(camera: string, minutes: number, label: string) {
    // Writing a long video takes a while (with the faces drawn into every frame: about 15 seconds per minute of video on a normal computer).
    return this.control.request<{ saved: number; clips: RawClip[]; note?: string }>("save", { camera, minutes, label }, 600000);
  }

  recordings(limit: number) {
    return this.control.request<{ folder: string; total: number; files: RawRecording[] }>("recordings", { limit });
  }

  deviceAdd(role: "camera" | "display", name: string) {
    return this.control.request<{ name: string; role: string; key: string; key_id: string }>("device_add", { role, name });
  }

  deviceRemove(name: string) {
    return this.control.request<{ removed: string }>("device_remove", { name });
  }

  stop() {
    return this.control.request<{ stopping: boolean }>("stop");
  }

  snapshot(camera: string, after: number) {
    return this.stream.request<RawSnapshot>("snapshot", { camera, after }, 5000);
  }

  disconnect() {
    this.control.disconnect();
    this.stream.disconnect();
  }
}
