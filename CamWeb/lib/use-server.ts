"use client";

import { useCallback, useEffect, useState } from "react";
import { getBackend } from "./backend";
import { usePreferences } from "./preferences";
import type { ServerStatus } from "./types";

/** The status of the server, refreshed in the interval of the connection settings. */
export function useServerStatus() {
  const { refreshSeconds } = usePreferences();
  const [status, setStatus] = useState<ServerStatus | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setStatus(await getBackend().getStatus());
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  useEffect(() => {
    const first = setTimeout(refresh, 0);
    const timer = setInterval(refresh, Math.max(refreshSeconds, 1) * 1000);
    return () => {
      clearTimeout(first);
      clearInterval(timer);
    };
  }, [refresh, refreshSeconds]);

  return { status, error, refresh };
}
