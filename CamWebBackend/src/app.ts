import fs from "node:fs";
import path from "node:path";
import cookie from "@fastify/cookie";
import rateLimit from "@fastify/rate-limit";
import fastifyStatic from "@fastify/static";
import Fastify, { type FastifyInstance } from "fastify";
import { LoginChecker, TokenSigner } from "./auth.ts";
import { CamServerApi } from "./camserver/api.ts";
import { CamServerClient } from "./camserver/client.ts";
import type { Config } from "./config.ts";
import { Overview } from "./overview.ts";
import { registerRoutes } from "./routes.ts";
import { SettingsStore } from "./settings-store.ts";
import { StreamHub } from "./streams.ts";

export interface App {
  server: FastifyInstance;
  shutdown(): Promise<void>;
}

export async function buildApp(config: Config, secret: Buffer, options: { logger?: boolean } = {}): Promise<App> {
  const server = Fastify({
    // Behind the Apache reverse proxy: the address, host and protocol of the visitor come from its X-Forwarded-* headers.
    trustProxy: true,
    bodyLimit: 64 * 1024,
    logger: options.logger === false ? false : { serializers: { req: (request) => ({ method: request.method, url: String(request.url).split("?")[0], remoteAddress: request.ip }) } },
  });

  await server.register(cookie);
  await server.register(rateLimit, { global: false });

  // A page of another site must not be able to give commands: the cookie is "SameSite=Strict", and a request, which a browser marks with another origin, is
  // refused too.
  server.addHook("onRequest", async (request, reply) => {
    if (request.method === "GET" || request.method === "HEAD" || request.method === "OPTIONS") {
      return;
    }

    const origin = request.headers.origin;
    if (origin) {
      let host = "";
      try {
        host = new URL(origin).host;
      } catch {
        // not an address
      }

      if (host !== request.host) {
        return reply.status(403).send({ error: "The request comes from another site." });
      }
    }
  });

  const settings = new SettingsStore(config);
  const api = new CamServerApi(new CamServerClient(() => settings.get()), new CamServerClient(() => settings.get()));
  settings.onChange(() => api.disconnect());

  const overview = new Overview(api);
  const hub = new StreamHub(
    api,
    { fps: config.streamFps, maxViewers: config.maxViewersPerCamera },
    (camera, jpeg) => overview.rememberPicture(camera, jpeg),
    { warn: (message) => server.log.warn(message) },
  );

  registerRoutes(server, {
    config,
    settings,
    api,
    overview,
    hub,
    signer: new TokenSigner(secret),
    login: new LoginChecker(config.adminUser, config.adminPasswordHash),
  });

  // The built web UI (CamWeb), if there is one.
  if (config.staticDir) {
    // The pages are files like dashboard.html, which are opened as /dashboard (next to a folder dashboard/ with the pages below it).
    server.addHook("onRequest", async (request, reply) => {
      if ((request.method !== "GET" && request.method !== "HEAD") || request.url.startsWith("/api/")) {
        return;
      }

      let route: string;
      try {
        route = decodeURIComponent(request.url.split("?")[0] ?? "").replace(/\/+$/, "");
      } catch {
        return;
      }

      const page = path.resolve(config.staticDir, `.${route}.html`);
      if (route && page.startsWith(config.staticDir + path.sep) && fs.existsSync(page)) {
        return reply.sendFile(path.relative(config.staticDir, page).split(path.sep).join("/"));
      }
    });

    await server.register(fastifyStatic, {
      root: config.staticDir,
      setHeaders: (response, filePath) => {
        // The files in _next/static have the hash of their content in the name.
        response.header("Cache-Control", filePath.includes(`${path.sep}_next${path.sep}static${path.sep}`) ? "public, max-age=31536000, immutable" : "no-cache");
      },
    });
  }

  server.setNotFoundHandler((request, reply) => {
    if (request.url.startsWith("/api/") || !config.staticDir) {
      return reply.status(404).send({ error: "Not found." });
    }

    const notFound = path.join(config.staticDir, "404.html");
    return fs.existsSync(notFound) ? reply.status(404).type("text/html").send(fs.createReadStream(notFound)) : reply.status(404).send({ error: "Not found." });
  });

  return {
    server,
    async shutdown() {
      hub.closeAll();
      api.disconnect();
      await server.close();
    },
  };
}
