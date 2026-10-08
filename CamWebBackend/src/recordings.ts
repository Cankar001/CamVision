import fs from "node:fs";
import path from "node:path";

/**
 * The file of a recording ("Front_door/20260101_120000_remote.avi", as CamServer lists it) inside the recordings folder. Null, if the name leads out of the
 * folder, is not a video, or the file is not there.
 */
export function resolveRecording(root: string, name: string): string | null {
  if (!root || !name || name.includes("\0") || path.extname(name).toLowerCase() !== ".avi") {
    return null;
  }

  const base = path.resolve(root);
  const file = path.resolve(base, name);
  if (!file.startsWith(base + path.sep)) {
    return null;
  }

  try {
    return fs.statSync(file).isFile() ? file : null;
  } catch {
    return null;
  }
}
