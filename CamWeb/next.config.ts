import type { NextConfig } from "next";
import { PHASE_DEVELOPMENT_SERVER } from "next/constants";

// The backend (CamWebBackend), which the page talks to: in development through this server (so the page and the API have the same origin, as behind the
// reverse proxy later).
const BACKEND_URL = process.env.BACKEND_URL ?? "http://127.0.0.1:3001";

export default function config(phase: string): NextConfig {
  const base: NextConfig = {
    experimental: {
      agentFeedback: true,
    },
    // cacheComponents and partialPrefetching (the defaults of the template) cannot be used with a static export, and this app is all client side anyway.
  };

  if (phase === PHASE_DEVELOPMENT_SERVER) {
    return {
      ...base,
      async rewrites() {
        return [{ source: "/api/:path*", destination: `${BACKEND_URL}/api/:path*` }];
      },
    };
  }

  // The whole page runs in the browser, so the build is a folder of static files (out/), which the backend serves.
  return { ...base, output: "export" };
}
