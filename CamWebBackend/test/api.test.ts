import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, test } from "node:test";
import { hashPassword, TokenSigner } from "../src/auth.ts";
import { buildApp, type App } from "../src/app.ts";
import type { Config } from "../src/config.ts";
import { jpegSize } from "../src/jpeg.ts";
import { resolveRecording } from "../src/recordings.ts";
import { FAKE_JPEG, FakeCamServer, TOKEN } from "./fake-camserver.ts";

let fake: FakeCamServer;
let app: App;
let dataDir: string;
let cookie = "";
let baseUrl = "";

before(async () => {
  fake = new FakeCamServer();
  await fake.ready();
  dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "camweb-test-"));

  const config: Config = {
    host: "127.0.0.1",
    port: 0,
    dataDir,
    adminUser: "admin",
    adminPasswordHash: await hashPassword("correct horse"),
    sessionHours: 1,
    camServer: { host: "127.0.0.1", port: fake.port, secure: false, token: TOKEN, tokenFile: "" },
    recordingsDir: "",
    staticDir: "",
    streamFps: 20,
    maxViewersPerCamera: 2,
  };

  app = await buildApp(config, Buffer.alloc(32, 7), { logger: false });
  baseUrl = await app.server.listen({ host: "127.0.0.1", port: 0 });
});

after(async () => {
  await app.shutdown();
  await fake.close();
  fs.rmSync(dataDir, { recursive: true, force: true });
});

const call = (method: "GET" | "POST" | "PUT" | "DELETE", url: string, payload?: unknown, headers: Record<string, string> = {}) =>
  app.server.inject({ method, url, payload: payload as object, headers: { cookie, ...headers } });

describe("login", () => {
  test("the API needs a login", async () => {
    assert.equal((await app.server.inject({ method: "GET", url: "/api/status" })).statusCode, 401);
  });

  test("a wrong password is refused, the right one gives a session", async () => {
    const wrong = await app.server.inject({ method: "POST", url: "/api/login", payload: { username: "admin", password: "nope" } });
    assert.equal(wrong.statusCode, 401);

    const wrongUser = await app.server.inject({ method: "POST", url: "/api/login", payload: { username: "root", password: "correct horse" } });
    assert.equal(wrongUser.statusCode, 401);

    const wrongCase = await app.server.inject({ method: "POST", url: "/api/login", payload: { username: "admin", password: "Correct horse" } });
    assert.equal(wrongCase.statusCode, 401);

    // The user name may be written in any case.
    const good = await app.server.inject({ method: "POST", url: "/api/login", payload: { username: " Admin ", password: "correct horse" } });
    assert.equal(good.statusCode, 200);
    const setCookie = String(good.headers["set-cookie"]);
    assert.match(setCookie, /HttpOnly/i);
    assert.match(setCookie, /SameSite=Strict/i);
    cookie = setCookie.split(";")[0]!;

    const me = await call("GET", "/api/me");
    assert.deepEqual(me.json(), { user: "admin" });
  });

  test("a forged or expired session is refused", async () => {
    const forged = new TokenSigner(Buffer.alloc(32, 1)).sign({ k: "session", u: "admin", exp: Math.floor(Date.now() / 1000) + 600 });
    const expired = new TokenSigner(Buffer.alloc(32, 7)).sign({ k: "session", u: "admin", exp: Math.floor(Date.now() / 1000) - 5 });
    for (const bad of [forged, expired]) {
      const response = await app.server.inject({ method: "GET", url: "/api/me", headers: { cookie: `camvision_session=${bad}` } });
      assert.equal(response.statusCode, 401);
    }
  });

  test("a request from another site is refused", async () => {
    const response = await call("POST", "/api/stop", {}, { origin: "https://evil.example", host: "cam.home" });
    assert.equal(response.statusCode, 403);
    assert.ok(!fake.commands.includes("stop"));
  });
});

describe("overview and commands", () => {
  test("status puts cameras, displays and devices together", async () => {
    const response = await call("GET", "/api/status");
    assert.equal(response.statusCode, 200);
    const status = response.json();
    assert.equal(status.bufferMinutes, 5);
    assert.deepEqual(status.cameras[0], {
      name: "Front door",
      address: "192.168.1.21:5000",
      fps: 15,
      width: 32,
      height: 16,
      bufferedSeconds: 20,
      lastSeenMsAgo: 40,
      facesVisible: 1,
    });
    assert.equal(status.displays[0].maxFps, 30);
    const online = Object.fromEntries(status.devices.map((device: { name: string; online: boolean }) => [device.name, device.online]));
    assert.deepEqual(online, { "Front door": true, "Living room": true, Basement: false });
  });

  test("save returns the files, and checks the arguments", async () => {
    const response = await call("POST", "/api/save", { camera: "Front door", minutes: 3 });
    assert.equal(response.statusCode, 200);
    assert.deepEqual(response.json().files, ["recordings/Front_door/20260101_120000_web.avi"]);
    assert.equal((await call("POST", "/api/save", { camera: "Front door", minutes: 0 })).statusCode, 400);
    assert.equal((await call("POST", "/api/save", { minutes: "5" })).statusCode, 400);
  });

  test("recordings are listed with the camera and the time", async () => {
    const response = await call("GET", "/api/recordings");
    const { recordings, downloadable } = response.json();
    assert.equal(downloadable, false);
    assert.deepEqual(recordings[0], { file: "Front_door/20260101_120000_web.avi", camera: "Front_door", sizeMegabytes: 5, time: "2026-01-01T12:00:00.000Z" });
    assert.equal((await call("GET", "/api/recordings/file?name=Front_door/20260101_120000_web.avi")).statusCode, 404);
  });

  test("devices can be added (the key is shown once) and removed", async () => {
    const added = await call("POST", "/api/devices", { role: "display", name: "Kitchen" });
    assert.equal(added.statusCode, 200);
    assert.equal(added.json().key, "ab".repeat(32));
    assert.equal((await call("POST", "/api/devices", { role: "toaster", name: "x" })).statusCode, 400);

    const removed = await call("DELETE", "/api/devices/Basement");
    assert.equal(removed.statusCode, 200);
    const status = (await call("GET", "/api/status")).json();
    assert.ok(!status.devices.some((device: { name: string }) => device.name === "Basement"));
  });

  test("stop is passed on", async () => {
    assert.equal((await call("POST", "/api/stop", {})).statusCode, 200);
    assert.ok(fake.commands.includes("stop"));
  });
});

describe("the connection to CamServer", () => {
  test("the token is never given back", async () => {
    const response = await call("GET", "/api/connection");
    const connection = response.json();
    assert.equal(connection.tokenSet, true);
    assert.equal(connection.tokenSource, "env");
    assert.equal(connection.state, "connected");
    assert.ok(!JSON.stringify(connection).includes(TOKEN));
  });

  test("a test with a wrong token says so", async () => {
    const response = await call("POST", "/api/connection/test", { host: "127.0.0.1", port: fake.port, secure: false, token: "wrong" });
    assert.equal(response.json().ok, false);
    assert.match(response.json().error, /token/);

    const good = await call("POST", "/api/connection/test", { host: "127.0.0.1", port: fake.port, secure: false });
    assert.equal(good.json().ok, true);
  });

  test("another server needs its own token, the saved one is not reused", async () => {
    const response = await call("PUT", "/api/connection", { host: "other.example", port: 45651, secure: false });
    assert.equal(response.statusCode, 400);
  });

  test("an unreachable server gives an error, not a crash", async () => {
    const saved = await call("PUT", "/api/connection", { host: "127.0.0.1", port: 1, secure: false, token: "x" });
    assert.equal(saved.statusCode, 200);
    assert.ok(fs.statSync(path.join(dataDir, "connection.json")).isFile());
    const status = await call("GET", "/api/status");
    assert.equal(status.statusCode, 503);
    assert.match(status.json().error, /not reachable/);

    await call("PUT", "/api/connection", { host: "127.0.0.1", port: fake.port, secure: false, token: TOKEN });
    assert.equal((await call("GET", "/api/status")).statusCode, 200);
  });
});

describe("the live stream", () => {
  async function streamUrl(camera: string) {
    const response = await call("POST", "/api/streams", { camera });
    return response;
  }

  test("there is no link for a camera, which is not connected", async () => {
    assert.equal((await streamUrl("Garage")).statusCode, 400);
  });

  test("a link gives an MJPEG stream of the pictures", async () => {
    const link = (await streamUrl("Front door")).json();
    assert.equal(link.format, "mjpeg");

    const received = await new Promise<Buffer>((resolve, reject) => {
      http.get(baseUrl + link.url, (response) => {
        assert.equal(response.statusCode, 200);
        assert.match(String(response.headers["content-type"]), /multipart\/x-mixed-replace; boundary=frame/);
        const chunks: Buffer[] = [];
        response.on("data", (chunk: Buffer) => {
          chunks.push(chunk);
          if (Buffer.concat(chunks).includes(FAKE_JPEG)) {
            response.destroy();
            resolve(Buffer.concat(chunks));
          }
        });
        response.on("error", reject);
      }).on("error", reject);
    });

    assert.ok(received.includes("--frame\r\nContent-Type: image/jpeg"));
    assert.ok(received.includes(FAKE_JPEG));
  });

  test("a changed or foreign link is refused", async () => {
    const link = (await streamUrl("Front door")).json();
    const tampered = await app.server.inject({ method: "GET", url: link.url.slice(0, -2) + "xx" });
    assert.equal(tampered.statusCode, 401);

    const other = await app.server.inject({ method: "GET", url: link.url.replace("Front%20door", "Garage") });
    assert.equal(other.statusCode, 401);

    // A login cookie is not a stream link.
    const withCookie = await call("GET", "/api/stream/Front%20door");
    assert.equal(withCookie.statusCode, 401);
  });

  test("the stream ends when the camera is gone", async () => {
    const link = (await streamUrl("Front door")).json();
    const ended = new Promise<void>((resolve, reject) => {
      http.get(baseUrl + link.url, (response) => {
        response.resume();
        response.on("end", resolve);
        response.on("error", reject);
      }).on("error", reject);
    });

    await new Promise((resolve) => setTimeout(resolve, 150));
    fake.cameraConnected = false;
    await ended;
    fake.cameraConnected = true;
  });
});

describe("helpers", () => {
  test("the size is read from a JPEG header", () => {
    assert.deepEqual(jpegSize(FAKE_JPEG), { width: 32, height: 16 });
    assert.equal(jpegSize(Buffer.from("not a picture")), null);
  });

  test("a recording outside of the folder is not found", () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "camweb-rec-"));
    fs.mkdirSync(path.join(root, "cam"));
    fs.writeFileSync(path.join(root, "cam", "a.avi"), "x");
    fs.writeFileSync(path.join(path.dirname(root), "secret.avi"), "x");
    try {
      assert.ok(resolveRecording(root, "cam/a.avi"));
      assert.equal(resolveRecording(root, "../secret.avi"), null);
      assert.equal(resolveRecording(root, "cam/../../secret.avi"), null);
      assert.equal(resolveRecording(root, "cam/a.txt"), null);
      assert.equal(resolveRecording("", "cam/a.avi"), null);
    } finally {
      fs.rmSync(root, { recursive: true, force: true });
      fs.rmSync(path.join(path.dirname(root), "secret.avi"), { force: true });
    }
  });
});
