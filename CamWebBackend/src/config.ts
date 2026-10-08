import fs from "node:fs";
import path from "node:path";

export interface Config {
  host: string;
  port: number;
  dataDir: string;
  adminUser: string;
  adminPasswordHash: string;
  sessionHours: number;
  /** The first values of the connection to CamServer (the web UI changes them later, see SettingsStore). */
  camServer: {
    host: string;
    port: number;
    secure: boolean;
    token: string;
    tokenFile: string;
  };
  recordingsDir: string;
  staticDir: string;
  streamFps: number;
  maxViewersPerCamera: number;
}

function text(env: NodeJS.ProcessEnv, name: string, fallback = ""): string {
  const value = env[name]?.trim();
  return value ? value : fallback;
}

function integer(env: NodeJS.ProcessEnv, name: string, fallback: number, min: number, max: number): number {
  const raw = text(env, name);
  if (!raw) {
    return fallback;
  }

  const value = Number(raw);
  if (!Number.isInteger(value) || value < min || value > max) {
    throw new Error(`${name} must be a whole number between ${min} and ${max} (it is "${raw}")`);
  }

  return value;
}

function flag(env: NodeJS.ProcessEnv, name: string, fallback: boolean): boolean {
  const raw = text(env, name).toLowerCase();
  if (!raw) {
    return fallback;
  }

  if (["true", "1", "yes", "on"].includes(raw)) {
    return true;
  }

  if (["false", "0", "no", "off"].includes(raw)) {
    return false;
  }

  throw new Error(`${name} must be true or false (it is "${raw}")`);
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const staticDir = path.resolve(text(env, "STATIC_DIR", "../CamWeb/out"));
  const recordingsDir = text(env, "RECORDINGS_DIR");

  return {
    host: text(env, "HOST", "127.0.0.1"),
    port: integer(env, "PORT", 3001, 1, 65535),
    dataDir: path.resolve(text(env, "DATA_DIR", "./data")),
    adminUser: text(env, "ADMIN_USER", "admin"),
    adminPasswordHash: text(env, "ADMIN_PASSWORD_HASH"),
    sessionHours: integer(env, "SESSION_HOURS", 12, 1, 24 * 30),
    camServer: {
      host: text(env, "CAMSERVER_HOST", "127.0.0.1"),
      port: integer(env, "CAMSERVER_PORT", 45651, 1, 65535),
      secure: flag(env, "CAMSERVER_SECURE", false),
      token: text(env, "CAMSERVER_TOKEN"),
      tokenFile: text(env, "CAMSERVER_TOKEN_FILE"),
    },
    recordingsDir: recordingsDir ? path.resolve(recordingsDir) : "",
    staticDir: fs.existsSync(staticDir) ? staticDir : "",
    streamFps: integer(env, "STREAM_FPS", 10, 1, 30),
    maxViewersPerCamera: integer(env, "MAX_VIEWERS_PER_CAMERA", 8, 1, 100),
  };
}
