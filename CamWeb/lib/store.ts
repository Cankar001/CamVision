"use client";

import { useSyncExternalStore } from "react";

/**
 * A tiny store on top of the browser storage, read with useSyncExternalStore. On the server (and during hydration) the fallback is used, so the
 * markup of the server and of the first client render are the same.
 */
export function createStorageStore<T>(key: string, fallback: T, storage: () => Storage) {
  const listeners = new Set<() => void>();
  let cachedRaw: string | null | undefined;
  let cachedValue: T = fallback;

  function read(): T {
    const raw = storage().getItem(key);
    if (raw === cachedRaw) {
      return cachedValue;
    }

    cachedRaw = raw;
    try {
      const parsed = raw === null ? fallback : JSON.parse(raw);
      cachedValue =
        typeof fallback === "object" && fallback !== null && typeof parsed === "object" && parsed !== null
          ? ({ ...fallback, ...parsed } as T)
          : (parsed as T);
    } catch {
      cachedValue = fallback;
    }

    return cachedValue;
  }

  function subscribe(listener: () => void) {
    listeners.add(listener);
    window.addEventListener("storage", listener);
    return () => {
      listeners.delete(listener);
      window.removeEventListener("storage", listener);
    };
  }

  return {
    use: () => useSyncExternalStore(subscribe, read, () => fallback),
    get: read,
    set(value: T) {
      storage().setItem(key, JSON.stringify(value));
      listeners.forEach((listener) => listener());
    },
    clear() {
      storage().removeItem(key);
      listeners.forEach((listener) => listener());
    },
  };
}

const noopSubscribe = () => () => {};

/** False on the server and during hydration, true afterwards in the browser. */
export function useHydrated(): boolean {
  return useSyncExternalStore(noopSubscribe, () => true, () => false);
}
