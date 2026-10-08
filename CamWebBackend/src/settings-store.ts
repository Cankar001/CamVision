import fs from "node:fs";
import path from "node:path";
import type { Config } from "./config.ts";

/** How to reach the remote control of CamServer. */
export interface ConnectionSettings {
  host: string;
  port: number;
  secure: boolean;
  token: string;
}

export type TokenSource = "settings" | "env" | "file" | "none";

interface StoredSettings {
  host?: string;
  port?: number;
  secure?: boolean;
  token?: string;
}

/**
 * The connection settings: the values of the environment are the start, what is saved in the web UI (data/connection.json) wins. The token is only
 * ever sent to CamServer, never back to the browser.
 */
export class SettingsStore {
  private readonly file: string;
  private stored: StoredSettings = {};
  private readonly listeners = new Set<() => void>();

  constructor(private readonly config: Config) {
    this.file = path.join(config.dataDir, "connection.json");
    try {
      this.stored = JSON.parse(fs.readFileSync(this.file, "utf8")) as StoredSettings;
    } catch {
      // nothing saved yet (or unreadable: the values of the environment are used)
    }
  }

  get(): ConnectionSettings {
    const { host, port, secure } = this.target();
    return { host, port, secure, token: this.resolveToken().token };
  }

  /** Everything except the token, and where the token comes from. */
  describe(): { host: string; port: number; secure: boolean; tokenSet: boolean; tokenSource: TokenSource } {
    const { token, source } = this.resolveToken();
    return { ...this.target(), tokenSet: token !== "", tokenSource: source };
  }

  /** Saves new settings. An empty token keeps the one there is. */
  update(next: { host: string; port: number; secure: boolean; token?: string }) {
    const stored: StoredSettings = { host: next.host, port: next.port, secure: next.secure };
    const token = next.token?.trim();
    if (token) {
      stored.token = token;
    } else if (this.stored.token) {
      stored.token = this.stored.token;
    }

    fs.mkdirSync(this.config.dataDir, { recursive: true });
    // The token is a secret: only the owner may read the file.
    fs.writeFileSync(this.file, JSON.stringify(stored, null, 2) + "\n", { mode: 0o600 });
    this.stored = stored;
    this.listeners.forEach((listener) => listener());
  }

  onChange(listener: () => void) {
    this.listeners.add(listener);
  }

  private target() {
    return {
      host: this.stored.host ?? this.config.camServer.host,
      port: this.stored.port ?? this.config.camServer.port,
      secure: this.stored.secure ?? this.config.camServer.secure,
    };
  }

  private resolveToken(): { token: string; source: TokenSource } {
    if (this.stored.token) {
      return { token: this.stored.token, source: "settings" };
    }

    if (this.config.camServer.token) {
      return { token: this.config.camServer.token, source: "env" };
    }

    if (this.config.camServer.tokenFile) {
      try {
        const token = fs.readFileSync(this.config.camServer.tokenFile, "utf8").trim();
        if (token) {
          return { token, source: "file" };
        }
      } catch {
        // the file is not there (yet)
      }
    }

    return { token: "", source: "none" };
  }
}
