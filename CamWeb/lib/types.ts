/** The data of the backend (CamWebBackend, which talks to the remote control of CamServer), as the UI uses it. */

export interface Camera {
  name: string;
  address: string;
  fps: number;
  /** null, as long as the backend has not seen a picture of the camera. */
  width: number | null;
  height: number | null;
  /** How many seconds of video are buffered on the server. */
  bufferedSeconds: number;
  /** How long ago the server heard from the camera last. */
  lastSeenMsAgo: number;
  facesVisible: number;
}

export interface Display {
  name: string;
  address: string;
  /** The camera it shows, empty for all cameras. */
  camera: string;
  maxFps: number;
}

export interface Device {
  name: string;
  role: "camera" | "display";
  keyId: string;
  online: boolean;
}

export interface NewDevice {
  name: string;
  role: "camera" | "display";
  /** The secret of the device. Shown once, the backend does not keep it. */
  key: string;
  keyId: string;
}

export interface Recording {
  file: string;
  camera: string;
  sizeMegabytes: number;
  time: string;
}

export interface RecordingList {
  recordings: Recording[];
  total: number;
  /** True, if the backend knows the recordings folder, so the files can be downloaded. */
  downloadable: boolean;
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

export interface SaveResult {
  files: string[];
  /** The cameras, whose video could not be saved, with the reason. */
  failed: string[];
}

/** The answer of the backend to a request for a live stream. */
export interface StreamLink {
  /** Where the live video is. */
  url: string;
  /** mjpeg: a continuous stream of pictures, shown in an image. video: a video, which the browser plays (mp4, webm). */
  format: "mjpeg" | "video";
}

/** How the backend reaches the remote control of CamServer. The token is never sent back to the browser. */
export interface ServerConnection {
  host: string;
  port: number;
  secure: boolean;
  tokenSet: boolean;
  /** Where the token comes from: saved in the web UI, the environment of the backend, or the token file of CamServer. */
  tokenSource: "settings" | "env" | "file" | "none";
  state: "idle" | "connecting" | "connected" | "error";
  error: string | null;
}

export interface ConnectionInput {
  host: string;
  port: number;
  secure: boolean;
  /** Empty keeps the saved token (only possible for the same server). */
  token: string;
}
