import type { ServerResponse } from "node:http";
import type { CamServerApi } from "./camserver/api.ts";
import { CamServerError } from "./camserver/client.ts";

// If the camera sends nothing new for this long, the stream ends (the browser shows that, instead of a frozen picture).
const STALL_END_MS = 10000;
// So many failed requests in a row end the stream.
const MAX_FAILURES = 3;

export class StreamLimitError extends Error {}

export interface Feed {
  camera: string;
  createdAt: number;
  viewers: Set<ServerResponse>;
  latest: Buffer;
  frame: number;
  lastChange: number;
  failures: number;
  timer: NodeJS.Timeout | null;
  ended: boolean;
}

const PART_HEADERS = (length: number) => `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${length}\r\n\r\n`;

/**
 * The live view: one loop per watched camera asks CamServer for the newest picture (the "snapshot" command), and sends every picture to all viewers
 * of that camera as MJPEG (multipart/x-mixed-replace, which a browser shows in a plain image). The loop only runs while somebody watches.
 */
export class StreamHub {
  private readonly feeds = new Map<string, Promise<Feed>>();

  constructor(
    private readonly api: CamServerApi,
    private readonly options: { fps: number; maxViewers: number },
    private readonly onPicture: (camera: string, jpeg: Buffer) => void,
    private readonly log: { warn(message: string): void },
  ) {}

  /** Makes sure the camera is watched (gets its first picture), before a viewer is attached. Fails if the camera is not there. */
  async acquire(camera: string): Promise<Feed> {
    let pending = this.feeds.get(camera);
    if (!pending) {
      pending = this.start(camera);
      this.feeds.set(camera, pending);
      pending.catch(() => this.feeds.delete(camera));
    }

    const feed = await pending;
    if (feed.ended) {
      return this.acquire(camera);
    }

    if (feed.viewers.size >= this.options.maxViewers) {
      throw new StreamLimitError(`Too many people watch "${camera}" already (at most ${this.options.maxViewers}).`);
    }

    return feed;
  }

  /** Sends the stream to the response of a viewer. It ends when the viewer leaves, or the stream ends. */
  attach(feed: Feed, response: ServerResponse) {
    response.writeHead(200, {
      "Content-Type": "multipart/x-mixed-replace; boundary=frame",
      "Cache-Control": "no-store",
      Connection: "keep-alive",
      "X-Accel-Buffering": "no",
    });
    response.socket?.setNoDelay(true);

    if (feed.ended) {
      response.end();
      return;
    }

    feed.viewers.add(response);
    this.write(response, feed.latest);
    response.on("close", () => {
      feed.viewers.delete(response);
      if (feed.viewers.size === 0) {
        this.end(feed);
      }
    });
  }

  closeAll() {
    for (const pending of this.feeds.values()) {
      pending.then((feed) => this.end(feed)).catch(() => {});
    }
  }

  private async start(camera: string): Promise<Feed> {
    const first = await this.api.snapshot(camera, 0);
    if (!first.jpeg) {
      throw new CamServerError(`There is no picture of the camera "${camera}".`, "command");
    }

    const feed: Feed = {
      camera,
      createdAt: Date.now(),
      viewers: new Set(),
      latest: Buffer.from(first.jpeg, "base64"),
      frame: first.frame,
      lastChange: Date.now(),
      failures: 0,
      timer: null,
      ended: false,
    };

    this.onPicture(camera, feed.latest);
    this.schedule(feed, 0);
    return feed;
  }

  private schedule(feed: Feed, delay: number) {
    feed.timer = setTimeout(() => void this.poll(feed), delay);
  }

  private async poll(feed: Feed) {
    if (feed.ended) {
      return;
    }

    const started = Date.now();
    try {
      const snapshot = await this.api.snapshot(feed.camera, feed.frame);
      feed.failures = 0;
      if (!snapshot.unchanged && snapshot.jpeg) {
        feed.frame = snapshot.frame;
        feed.latest = Buffer.from(snapshot.jpeg, "base64");
        feed.lastChange = Date.now();
        this.onPicture(feed.camera, feed.latest);
        for (const viewer of feed.viewers) {
          this.write(viewer, feed.latest);
        }
      } else if (Date.now() - feed.lastChange > STALL_END_MS) {
        this.log.warn(`The camera "${feed.camera}" sends no pictures anymore, its stream ends.`);
        this.end(feed);
        return;
      }
    } catch (error) {
      feed.failures++;
      // "command": CamServer answered, that the camera is gone.
      if (feed.failures >= MAX_FAILURES || (error instanceof CamServerError && error.code === "command")) {
        this.log.warn(`The stream of "${feed.camera}" ends: ${(error as Error).message}`);
        this.end(feed);
        return;
      }
    }

    // Nobody came to watch (the viewer left before the stream started).
    if (feed.viewers.size === 0 && Date.now() - feed.createdAt > 5000) {
      this.end(feed);
      return;
    }

    if (!feed.ended) {
      this.schedule(feed, Math.max(0, 1000 / this.options.fps - (Date.now() - started)));
    }
  }

  private write(response: ServerResponse, jpeg: Buffer) {
    // A viewer with a slow connection skips pictures, instead of piling them up in memory.
    if (response.destroyed || response.writableNeedDrain) {
      return;
    }

    response.write(PART_HEADERS(jpeg.length));
    response.write(jpeg);
    response.write("\r\n");
  }

  private end(feed: Feed) {
    if (feed.ended) {
      return;
    }

    feed.ended = true;
    if (feed.timer) {
      clearTimeout(feed.timer);
    }

    this.feeds.delete(feed.camera);
    for (const viewer of feed.viewers) {
      viewer.end();
    }

    feed.viewers.clear();
  }
}
