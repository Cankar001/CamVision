"use client";

import { createStorageStore } from "./store";

/** Settings of this browser (not of the server). */
export interface Preferences {
  /** Seconds between two refreshes of the overview. */
  refreshSeconds: number;
}

export const DEFAULT_PREFERENCES: Preferences = { refreshSeconds: 2 };

const preferencesStore = createStorageStore<Preferences>("camvision.preferences", DEFAULT_PREFERENCES, () => localStorage);

export const usePreferences = preferencesStore.use;
export const savePreferences = preferencesStore.set;
