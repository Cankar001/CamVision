import fs from "node:fs";
import { loadOrCreateSecret } from "./auth.ts";
import { buildApp } from "./app.ts";
import { loadConfig } from "./config.ts";

let config;
try {
  config = loadConfig();
} catch (error) {
  console.error(`Wrong setting: ${(error as Error).message}`);
  process.exit(1);
}

if (!config.adminPasswordHash) {
  console.error("There is no login yet. Make the hash of a password with  npm run hash-password  and put it into .env as ADMIN_PASSWORD_HASH.");
  process.exit(1);
}

fs.mkdirSync(config.dataDir, { recursive: true });
const app = await buildApp(config, loadOrCreateSecret(config.dataDir));

for (const signal of ["SIGINT", "SIGTERM"] as const) {
  process.on(signal, () => {
    app.server.log.info(`${signal}: shutting down`);
    app.shutdown().finally(() => process.exit(0));
  });
}

try {
  await app.server.listen({ host: config.host, port: config.port });
  if (!config.staticDir) {
    app.server.log.info("No built web UI found (STATIC_DIR): only the API is served.");
  }

  if (config.host !== "127.0.0.1" && config.host !== "::1") {
    app.server.log.warn(`Listening on ${config.host}: the traffic is not encrypted here. Put the reverse proxy (HTTPS) in front of it, and keep HOST=127.0.0.1 if it is on the same computer.`);
  }
} catch (error) {
  app.server.log.error(error);
  process.exit(1);
}
