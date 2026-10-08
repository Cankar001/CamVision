import { api } from "./api";
import type { CamBackend } from "./backend";
import type { ConnectionInput, NewDevice, RecordingList, SaveResult, ServerConnection, ServerStatus, StreamLink } from "./types";

/** The backend over HTTP: the API of CamWebBackend. */
export class HttpBackend implements CamBackend {
  async login(username: string, password: string): Promise<string> {
    return (await api<{ user: string }>("POST", "/api/login", { username, password })).user;
  }

  async logout(): Promise<void> {
    await api("POST", "/api/logout", {});
  }

  getStatus(): Promise<ServerStatus> {
    return api("GET", "/api/status");
  }

  async saveRecording(camera: string, minutes: number): Promise<SaveResult> {
    const result = await api<{ files: string[]; failed: string[] }>("POST", "/api/save", { camera, minutes });
    return { files: result.files, failed: result.failed };
  }

  listRecordings(limit: number): Promise<RecordingList> {
    return api("GET", `/api/recordings?limit=${limit}`);
  }

  recordingDownloadUrl(file: string): string {
    return `/api/recordings/file?name=${encodeURIComponent(file)}`;
  }

  requestStream(camera: string): Promise<StreamLink> {
    return api("POST", "/api/streams", { camera });
  }

  addDevice(role: "camera" | "display", name: string): Promise<NewDevice> {
    return api("POST", "/api/devices", { role, name });
  }

  async removeDevice(name: string): Promise<void> {
    await api("DELETE", `/api/devices/${encodeURIComponent(name)}`);
  }

  async stopServer(): Promise<void> {
    await api("POST", "/api/stop", {});
  }

  getConnection(): Promise<ServerConnection> {
    return api("GET", "/api/connection");
  }

  saveConnection(input: ConnectionInput): Promise<ServerConnection> {
    return api("PUT", "/api/connection", input);
  }

  testConnection(input: ConnectionInput): Promise<{ ok: boolean; error: string | null }> {
    return api("POST", "/api/connection/test", input);
  }
}
