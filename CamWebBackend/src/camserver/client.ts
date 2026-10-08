import { WebSocket } from "ws";
import type { ConnectionSettings } from "../settings-store.ts";

export type CamServerErrorCode =
  /** The server could not be reached (not running, wrong address or port). */
  | "unreachable"
  /** There is no token, or the server did not accept it. */
  | "auth"
  /** No answer in time. */
  | "timeout"
  /** The connection ended while waiting for the answer. */
  | "closed"
  /** The server answered, but with an error ("there is no camera ..."). */
  | "command";

export class CamServerError extends Error {
  constructor(
    message: string,
    readonly code: CamServerErrorCode,
  ) {
    super(message);
    this.name = "CamServerError";
  }
}

export type ConnectionState = "idle" | "connecting" | "connected" | "error";

interface Pending {
  resolve(result: unknown): void;
  reject(error: CamServerError): void;
  timer: NodeJS.Timeout;
}

const CONNECT_TIMEOUT_MS = 5000;

/**
 * One connection to the remote control (WebSocket) of CamServer: logs in with the token, sends the commands as JSON and matches the answers by their id.
 * The connection is made when the first command is sent, and again when it is gone, so the server may be started later or restarted.
 *
 * The server answers one connection in the order of the requests, so a slow command (saving a video) holds up the others on the same connection. That is
 * why the live view has a connection of its own.
 */
export class CamServerClient {
  private socket: WebSocket | null = null;
  private connecting: Promise<WebSocket> | null = null;
  private readonly pending = new Map<number, Pending>();
  private nextId = 1;
  private currentState: ConnectionState = "idle";
  private currentError: string | null = null;

  constructor(private readonly getSettings: () => ConnectionSettings) {}

  get state(): ConnectionState {
    return this.currentState;
  }

  get lastError(): string | null {
    return this.currentError;
  }

  /** Sends a command, and returns its result. Fails with a CamServerError. */
  async request<T = Record<string, unknown>>(cmd: string, args: Record<string, unknown> = {}, timeoutMs = 10000): Promise<T> {
    const socket = await this.connect();
    return (await this.send(socket, cmd, args, timeoutMs)) as T;
  }

  /** Ends the connection (the next command makes a new one, with the current settings). */
  disconnect() {
    this.socket?.terminate();
    this.socket = null;
    this.connecting = null;
    this.failAll(new CamServerError("The connection was closed.", "closed"));
    this.currentState = "idle";
  }

  private connect(): Promise<WebSocket> {
    if (this.socket && this.socket.readyState === WebSocket.OPEN) {
      return Promise.resolve(this.socket);
    }

    this.connecting ??= this.open().finally(() => {
      this.connecting = null;
    });
    return this.connecting;
  }

  private async open(): Promise<WebSocket> {
    const settings = this.getSettings();
    if (!settings.token) {
      this.fail("There is no token for CamServer yet (set it on the Connection page).");
      throw new CamServerError(this.currentError!, "auth");
    }

    this.currentState = "connecting";
    const url = `${settings.secure ? "wss" : "ws"}://${settings.host.includes(":") && !settings.host.startsWith("[") ? `[${settings.host}]` : settings.host}:${settings.port}`;

    const socket = await new Promise<WebSocket>((resolve, reject) => {
      // Pictures of the live view come as one message: the default limit of the library (100 MB) is plenty.
      const ws = new WebSocket(url, { handshakeTimeout: CONNECT_TIMEOUT_MS });
      ws.once("open", () => resolve(ws));
      ws.once("error", (error) => reject(new CamServerError(`CamServer is not reachable at ${url} (${error.message}).`, "unreachable")));
    }).catch((error: CamServerError) => {
      this.fail(error.message);
      throw error;
    });

    socket.on("message", (data) => this.onMessage(data.toString()));
    socket.on("close", () => this.onClose(socket));
    socket.on("error", () => {
      // The close event follows, and fails everything, which waits.
    });

    this.socket = socket;
    try {
      await this.send(socket, "auth", { token: settings.token }, CONNECT_TIMEOUT_MS);
    } catch (error) {
      socket.terminate();
      this.socket = null;
      const message = error instanceof CamServerError && error.code === "command" ? `CamServer did not accept the token (${error.message}).` : (error as Error).message;
      this.fail(message);
      throw new CamServerError(message, "auth");
    }

    this.currentState = "connected";
    this.currentError = null;
    return socket;
  }

  private send(socket: WebSocket, cmd: string, args: Record<string, unknown>, timeoutMs: number): Promise<unknown> {
    return new Promise((resolve, reject) => {
      const id = this.nextId++;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new CamServerError(`CamServer did not answer "${cmd}" in time.`, "timeout"));
        // The late answer would come out of order, so the connection is not used anymore.
        if (this.socket === socket) {
          socket.terminate();
        }
      }, timeoutMs);

      this.pending.set(id, { resolve, reject, timer });
      socket.send(JSON.stringify({ id, cmd, args }), (error) => {
        if (error) {
          clearTimeout(timer);
          this.pending.delete(id);
          reject(new CamServerError(`Could not send "${cmd}" to CamServer (${error.message}).`, "closed"));
        }
      });
    });
  }

  private onMessage(text: string) {
    let message: { id?: number; ok?: boolean; result?: unknown; error?: string };
    try {
      message = JSON.parse(text);
    } catch {
      return;
    }

    // Events of the server ({"event": ...}) have no id, nothing waits for them.
    if (typeof message.id !== "number") {
      return;
    }

    const pending = this.pending.get(message.id);
    if (!pending) {
      return;
    }

    this.pending.delete(message.id);
    clearTimeout(pending.timer);
    if (message.ok) {
      pending.resolve(message.result ?? {});
    } else {
      pending.reject(new CamServerError(message.error ?? "unknown error", "command"));
    }
  }

  private onClose(socket: WebSocket) {
    if (this.socket === socket) {
      this.socket = null;
      if (this.currentState === "connected") {
        this.currentState = "idle";
      }
    }

    this.failAll(new CamServerError("The connection to CamServer ended.", "closed"));
  }

  private failAll(error: CamServerError) {
    for (const [id, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(error);
      this.pending.delete(id);
    }
  }

  private fail(message: string) {
    this.currentState = "error";
    this.currentError = message;
  }
}
