"use client";

import { createStorageStore } from "./store";

interface Session {
  user: string | null;
}

/**
 * Who is logged in, as far as this tab knows. The login itself is a cookie of the backend (which the page cannot read): if it ends, the next request is
 * answered with 401, and the session here ends too.
 */
const sessionStore = createStorageStore<Session>("camvision.session", { user: null }, () => sessionStorage);

export const useSession = sessionStore.use;

export function startSession(user: string) {
  sessionStore.set({ user });
}

export function endSession() {
  sessionStore.clear();
}
