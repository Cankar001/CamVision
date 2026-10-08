import type { ConnectionInput, NewDevice, RecordingList, SaveResult, ServerConnection, ServerStatus, StreamLink } from "./types";
import { HttpBackend } from "./http-backend";
import { MockBackend } from "./mock-backend";

/**
 * What the UI needs from the backend. The UI never talks to CamServer itself: the backend (CamWebBackend) does, and the HttpBackend asks the backend over
 * its API. The MockBackend answers with demo data, for working on the UI without a backend (NEXT_PUBLIC_BACKEND=mock).
 */
export interface CamBackend {
  /** Logs in, and returns the name of the user. Fails with the reason (wrong password, ...). */
  login(username: string, password: string): Promise<string>;
  logout(): Promise<void>;

  getStatus(): Promise<ServerStatus>;
  /** Saves the last minutes of the buffer of one camera (or all, if the camera is empty) as video files. */
  saveRecording(camera: string, minutes: number): Promise<SaveResult>;
  listRecordings(limit: number): Promise<RecordingList>;
  /** The address, from which a recording is downloaded. */
  recordingDownloadUrl(file: string): string;
  /** Asks for the live stream of one camera (the user opened it). The backend answers with the URL, which the UI then shows. Fails if the camera is gone. */
  requestStream(camera: string): Promise<StreamLink>;
  addDevice(role: "camera" | "display", name: string): Promise<NewDevice>;
  /** Takes the key of a device away, it is disconnected. */
  removeDevice(name: string): Promise<void>;
  /** Stops the server (the videos, which are being recorded, are finished first). */
  stopServer(): Promise<void>;

  getConnection(): Promise<ServerConnection>;
  saveConnection(input: ConnectionInput): Promise<ServerConnection>;
  /** Tries to log in at CamServer with these settings, without saving them. */
  testConnection(input: ConnectionInput): Promise<{ ok: boolean; error: string | null }>;
}

export const isMockBackend = process.env.NEXT_PUBLIC_BACKEND === "mock";

let backend: CamBackend | null = null;

export function getBackend(): CamBackend {
  backend ??= isMockBackend ? new MockBackend() : new HttpBackend();
  return backend;
}
