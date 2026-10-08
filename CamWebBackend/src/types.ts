/** What the API gives to the web UI (the same shapes as lib/types.ts of CamWeb). */

export interface Camera {
  name: string;
  address: string;
  fps: number;
  /** null, as long as no picture of the camera was seen (the size is read from the pictures). */
  width: number | null;
  height: number | null;
  bufferedSeconds: number;
  lastSeenMsAgo: number;
  facesVisible: number;
}

export interface Display {
  name: string;
  address: string;
  camera: string;
  maxFps: number;
}

export interface Device {
  name: string;
  role: "camera" | "display";
  keyId: string;
  online: boolean;
}

export interface Recording {
  file: string;
  camera: string;
  sizeMegabytes: number;
  time: string;
}

export interface ServerStatus {
  cameras: Camera[];
  displays: Display[];
  devices: Device[];
  bufferMinutes: number;
  deviceKeysRequired: boolean;
  recording: {
    folder: string;
    scheduled: boolean;
    schedule: string;
    defaultMinutes: number;
  };
}
