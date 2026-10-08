import fs from "node:fs";
import path from "node:path";
import type { FastifyInstance, FastifyReply, FastifyRequest } from "fastify";
import { LoginChecker, TokenSigner } from "./auth.ts";
import type { CamServerApi } from "./camserver/api.ts";
import { CamServerClient, CamServerError } from "./camserver/client.ts";
import type { Config } from "./config.ts";
import { Overview, toRecording } from "./overview.ts";
import { resolveRecording } from "./recordings.ts";
import type { SettingsStore } from "./settings-store.ts";
import { StreamHub, StreamLimitError } from "./streams.ts";

declare module "fastify" {
  interface FastifyRequest {
    user?: string;
  }
}

export const SESSION_COOKIE = "camvision_session";
// How long the link to a stream can be used to start it (a running stream is not cut off).
const STREAM_LINK_SECONDS = 60;

export interface Services {
  config: Config;
  settings: SettingsStore;
  api: CamServerApi;
  overview: Overview;
  hub: StreamHub;
  signer: TokenSigner;
  login: LoginChecker;
}

class BadRequest extends Error {}

function body(request: FastifyRequest): Record<string, unknown> {
  const value = request.body;
  return value && typeof value === "object" && !Array.isArray(value) ? (value as Record<string, unknown>) : {};
}

function text(source: Record<string, unknown>, key: string, max: number, required = true): string {
  const value = source[key];
  if (value === undefined || value === null || value === "") {
    if (required) {
      throw new BadRequest(`"${key}" is missing.`);
    }

    return "";
  }

  if (typeof value !== "string" || value.length > max) {
    throw new BadRequest(`"${key}" must be a text with at most ${max} characters.`);
  }

  return value;
}

function whole(source: Record<string, unknown>, key: string, min: number, max: number): number {
  const value = source[key];
  if (typeof value !== "number" || !Number.isInteger(value) || value < min || value > max) {
    throw new BadRequest(`"${key}" must be a whole number between ${min} and ${max}.`);
  }

  return value;
}

/** The HTTP status and text for a failure. */
function failure(error: unknown): { status: number; message: string } {
  if (error instanceof BadRequest) {
    return { status: 400, message: error.message };
  }

  if (error instanceof StreamLimitError) {
    return { status: 429, message: error.message };
  }

  if (error instanceof CamServerError) {
    const status = { unreachable: 503, auth: 502, timeout: 504, closed: 503, command: 400 }[error.code];
    return { status, message: error.message };
  }

  return { status: 500, message: "Something went wrong on the server." };
}

function validateTarget(source: Record<string, unknown>) {
  const host = text(source, "host", 255).trim();
  if (!/^[A-Za-z0-9._:\-[\]]+$/.test(host)) {
    throw new BadRequest("The host must be a name or an IP address (without ws:// or a path).");
  }

  const port = whole(source, "port", 1, 65535);
  const secure = source.secure === true;
  const token = text(source, "token", 500, false).trim();
  return { host, port, secure, token };
}

export function registerRoutes(app: FastifyInstance, services: Services) {
  const { config, settings, api, overview, hub, signer, login } = services;

  app.setErrorHandler((error, request, reply) => {
    const known = failure(error);
    if (known.status === 500) {
      // Errors of the framework (a body that is not JSON, too large) have their own status.
      const status = (error as { statusCode?: number }).statusCode;
      if (status && status >= 400 && status < 500) {
        return reply.status(status).send({ error: (error as Error).message });
      }

      request.log.error(error);
    }

    return reply.status(known.status).send({ error: known.message });
  });

  app.get("/api/health", async () => ({ ok: true }));

  // ---- the login

  app.post("/api/login", { config: { rateLimit: { max: 10, timeWindow: "1 minute" } } }, async (request, reply) => {
    const data = body(request);
    const username = typeof data.username === "string" ? data.username : "";
    const password = typeof data.password === "string" ? data.password : "";

    const result = await login.check(username, password);
    if (!result.ok) {
      request.log.warn(result.userMatches ? "A login failed: the user name is right, the password is not." : "A login failed: the user name is not the one in ADMIN_USER.");
      // Guessing is made slow.
      await new Promise((resolve) => setTimeout(resolve, 500));
      return reply.status(401).send({ error: "Wrong user name or password." });
    }

    const token = signer.sign({ k: "session", u: config.adminUser, exp: Math.floor(Date.now() / 1000) + config.sessionHours * 3600 });
    reply.setCookie(SESSION_COOKIE, token, {
      httpOnly: true,
      sameSite: "strict",
      path: "/",
      secure: request.protocol === "https",
      maxAge: config.sessionHours * 3600,
    });
    return { user: config.adminUser };
  });

  app.post("/api/logout", async (_request, reply) => {
    reply.clearCookie(SESSION_COOKIE, { path: "/" });
    return { ok: true };
  });

  // ---- the live stream: no cookie, but the signed link from POST /api/streams (an image in the page cannot send headers)

  app.get("/api/stream/:camera", async (request: FastifyRequest<{ Params: { camera: string }; Querystring: { t?: string } }>, reply) => {
    const { camera } = request.params;
    const payload = signer.verify(request.query.t, "stream");
    if (!payload || payload.c !== camera) {
      return reply.status(401).send({ error: "The link to the stream is not valid (anymore). Ask for a new one." });
    }

    const feed = await hub.acquire(camera);
    reply.hijack();
    hub.attach(feed, reply.raw);
  });

  // ---- everything else needs the login

  app.register(async (secured) => {
    secured.addHook("preHandler", async (request: FastifyRequest, reply: FastifyReply) => {
      const payload = signer.verify(request.cookies[SESSION_COOKIE], "session");
      if (!payload) {
        return reply.status(401).send({ error: "Not logged in." });
      }

      request.user = String(payload.u);
    });

    secured.get("/api/me", async (request) => ({ user: request.user }));

    secured.get("/api/status", async () => overview.get());

    secured.post("/api/streams", async (request) => {
      const camera = text(body(request), "camera", 200);
      const status = await api.status();
      if (!status.cameras.some((entry) => entry.name === camera)) {
        throw new CamServerError(`There is no camera "${camera}" connected.`, "command");
      }

      const token = signer.sign({ k: "stream", c: camera, exp: Math.floor(Date.now() / 1000) + STREAM_LINK_SECONDS });
      return { url: `/api/stream/${encodeURIComponent(camera)}?t=${token}`, format: "mjpeg", expiresInSeconds: STREAM_LINK_SECONDS };
    });

    secured.post("/api/save", async (request) => {
      const data = body(request);
      const camera = text(data, "camera", 200, false);
      const minutes = whole(data, "minutes", 1, 1440);
      const result = await api.save(camera, minutes, "web");
      const files = result.clips.filter((clip) => clip.ok && clip.file).map((clip) => clip.file as string);
      if (files.length === 0) {
        const reasons = result.clips.map((clip) => `${clip.camera}: ${clip.error ?? "nothing saved"}`).join("; ");
        throw new CamServerError(reasons || result.note || "Nothing was saved.", "command");
      }

      const failed = result.clips.filter((clip) => !clip.ok).map((clip) => `${clip.camera}: ${clip.error ?? "failed"}`);
      return { files, failed, note: result.note ?? null };
    });

    secured.get("/api/recordings", async (request: FastifyRequest<{ Querystring: { limit?: string } }>) => {
      const limit = Math.min(Math.max(Number(request.query.limit) || 50, 1), 500);
      const result = await api.recordings(limit);
      return { recordings: result.files.map(toRecording), total: result.total, downloadable: config.recordingsDir !== "" };
    });

    secured.get("/api/recordings/file", async (request: FastifyRequest<{ Querystring: { name?: string } }>, reply) => {
      const file = resolveRecording(config.recordingsDir, request.query.name ?? "");
      if (!file) {
        return reply.status(404).send({ error: config.recordingsDir ? "There is no such recording." : "Downloads are off (RECORDINGS_DIR is not set)." });
      }

      reply.header("Content-Type", "video/x-msvideo");
      reply.header("Content-Disposition", `attachment; filename="${path.basename(file).replace(/["\\]/g, "_")}"`);
      return reply.send(fs.createReadStream(file));
    });

    secured.post("/api/devices", async (request) => {
      const data = body(request);
      const role = text(data, "role", 20);
      if (role !== "camera" && role !== "display") {
        throw new BadRequest('"role" must be camera or display.');
      }

      const name = text(data, "name", 64).trim();
      const device = await api.deviceAdd(role, name);
      // The key is shown once: it is not kept here.
      return { name: device.name, role: device.role, key: device.key, keyId: device.key_id };
    });

    secured.delete("/api/devices/:name", async (request: FastifyRequest<{ Params: { name: string } }>) => {
      await api.deviceRemove(request.params.name);
      return { removed: request.params.name };
    });

    secured.post("/api/stop", async () => {
      await api.stop();
      return { stopping: true };
    });

    // ---- the connection to CamServer

    secured.get("/api/connection", async () => ({
      ...settings.describe(),
      state: api.control.state,
      error: api.control.lastError,
    }));

    secured.put("/api/connection", async (request) => {
      const next = validateTarget(body(request));
      requireTokenForNewTarget(settings, next);
      settings.update(next);
      // The next command connects with the new settings.
      api.disconnect();
      return { ...settings.describe(), state: api.control.state, error: null };
    });

    secured.post("/api/connection/test", async (request) => {
      const next = validateTarget(body(request));
      requireTokenForNewTarget(settings, next);
      const token = next.token || settings.get().token;
      const client = new CamServerClient(() => ({ host: next.host, port: next.port, secure: next.secure, token }));
      try {
        await client.request("ping", {}, 5000);
        return { ok: true, error: null };
      } catch (error) {
        return { ok: false, error: error instanceof Error ? error.message : String(error) };
      } finally {
        client.disconnect();
      }
    });
  });
}

/** The saved token is only used for the server it was made for: another address needs its own token (or the saved token could be sent to anybody). */
function requireTokenForNewTarget(settings: SettingsStore, next: { host: string; port: number; secure: boolean; token: string }) {
  const current = settings.get();
  const same = current.host.toLowerCase() === next.host.toLowerCase() && current.port === next.port && current.secure === next.secure;
  if (!same && !next.token) {
    throw new BadRequest("For another server, enter its token too.");
  }
}
