import { WebSocketServer, type WebSocket } from "ws";

/** A JPEG header (no picture data) with the size 32x16: enough for the size to be read. */
export const FAKE_JPEG = Buffer.from([0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x20, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xd9]);

export const TOKEN = "test-token";

/** Plays the remote control of CamServer (the same messages), with fixed data, so the backend can be tested without the C++ server. */
export class FakeCamServer {
  private readonly wss: WebSocketServer;
  readonly commands: string[] = [];
  frame = 1;
  /** The picture it sends (the default has a header only). */
  jpeg: Buffer = FAKE_JPEG;
  cameraConnected = true;
  devices = [
    { name: "Front door", role: "camera", key_id: "aaaa1111" },
    { name: "Living room", role: "display", key_id: "bbbb2222" },
    { name: "Basement", role: "camera", key_id: "cccc3333" },
  ];

  constructor(port = 0) {
    this.wss = new WebSocketServer({ host: "127.0.0.1", port });
    this.wss.on("connection", (socket) => this.serve(socket));
  }

  get port(): number {
    return (this.wss.address() as { port: number }).port;
  }

  async ready() {
    if (!this.wss.address()) {
      await new Promise((resolve) => this.wss.once("listening", resolve));
    }
  }

  async close() {
    for (const client of this.wss.clients) {
      client.terminate();
    }

    await new Promise((resolve) => this.wss.close(resolve));
  }

  private serve(socket: WebSocket) {
    let authenticated = false;
    socket.on("message", (data) => {
      const request = JSON.parse(data.toString()) as { id: number; cmd: string; args: Record<string, unknown> };
      const answer = (result: unknown) => socket.send(JSON.stringify({ id: request.id, ok: true, result }));
      const fail = (error: string) => socket.send(JSON.stringify({ id: request.id, ok: false, error }));

      if (request.cmd === "auth") {
        if (request.args.token === TOKEN) {
          authenticated = true;
          return answer({ authenticated: true });
        }

        return fail("wrong token");
      }

      if (!authenticated) {
        return fail("not logged in");
      }

      this.commands.push(request.cmd);
      switch (request.cmd) {
        case "ping":
          return answer({ pong: true });
        case "status":
          return answer({
            cameras: this.cameraConnected
              ? [{ name: "Front door", address: "192.168.1.21:5000", frames_buffered: 300, seconds_buffered: 20, last_seen_ms_ago: 40, faces_visible: 1 }]
              : [],
            displays: 1,
            buffer_minutes: 5,
            device_keys_required: true,
            recording: { folder: "recordings", scheduled: false, schedule: "", default_minutes: 5 },
          });
        case "displays":
          return answer({ displays: [{ name: "Living room", address: "192.168.1.40:6000", camera: "", max_fps: 30 }] });
        case "devices":
          return answer({ devices: this.devices });
        case "save":
          return answer({ saved: 1, clips: [{ camera: "Front door", ok: true, frames: 300, file: "recordings/Front_door/20260101_120000_web.avi" }] });
        case "recordings":
          return answer({ folder: "recordings", total: 1, files: [{ file: "Front_door/20260101_120000_web.avi", bytes: 5 * 1024 * 1024, modified: 1767268800 }] });
        case "device_add":
          return answer({ name: request.args.name, role: request.args.role, key: "ab".repeat(32), key_id: "dddd4444" });
        case "device_remove":
          this.devices = this.devices.filter((device) => device.name !== request.args.name);
          return answer({ removed: request.args.name });
        case "stop":
          return answer({ stopping: true });
        case "snapshot":
          if (request.args.camera !== "Front door" || !this.cameraConnected) {
            return fail(`there is no picture of the camera "${request.args.camera}"`);
          }

          if (request.args.after === this.frame) {
            return answer({ camera: "Front door", frame: this.frame, unchanged: true });
          }

          return answer({ camera: "Front door", frame: this.frame, jpeg: this.jpeg.toString("base64") });
        default:
          return fail(`unknown command '${request.cmd}'`);
      }
    });
  }
}
