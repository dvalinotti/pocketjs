import {
  LOCALMEDIA,
  validLocalStatus,
  validLocalTrack,
  type LocalMediaOps,
  type LocalStatus,
  type LocalTrack,
} from "../../contracts/spec/localmedia.ts";
export { LOCALMEDIA };
export type { LocalMediaOps, LocalPhase, LocalStatus, LocalTrack } from "../../contracts/spec/localmedia.ts";

/** A texture handle (> 0), 0 for no art, or "pending" while the host decodes it. */
export type LocalArtwork = number | "pending";

export interface LocalMedia {
  scan(): boolean;
  tracks(): LocalTrack[];
  /** The open's serial (> 0), or 0 when the host refused the id. */
  open(id: number): number;
  pause(value: boolean): void;
  seek(ms: number): void;
  volume(value: number): void;
  status(): LocalStatus;
  artwork(id: number): LocalArtwork;
  releaseArtwork(handle: number): void;
}

function trackId(value: number): number {
  if (!Number.isInteger(value) || value < 0) throw new Error(`Invalid track id: ${value}`);
  return value;
}

/** The host's local media module (capability media.local). */
export function localMedia(ops = (globalThis as unknown as { localmedia?: LocalMediaOps }).localmedia): LocalMedia {
  if (!ops) throw new Error("Host does not implement media.local");
  // status() is read every frame: an unchanged reply returns the previous object, unparsed.
  let lastRaw = "";
  let lastStatus: LocalStatus | null = null;
  return {
    scan: () => ops.scan(),
    tracks() {
      const list = JSON.parse(ops.tracks()) as unknown;
      if (!Array.isArray(list) || !list.every(validLocalTrack)) throw new Error("Host returned a malformed track list");
      return list;
    },
    open: (id) => ops.open(trackId(id)),
    pause: (value) => ops.paused(value),
    seek: (ms) => ops.seek(Number.isFinite(ms) ? Math.max(0, Math.round(ms)) : 0),
    volume: (value) => ops.volume(Number.isFinite(value) ? Math.min(1, Math.max(0, value)) : 0),
    status() {
      const raw = ops.status();
      if (raw === lastRaw && lastStatus) return lastStatus;
      const status = JSON.parse(raw) as unknown;
      if (!validLocalStatus(status)) throw new Error("Host returned a malformed status");
      lastRaw = raw;
      lastStatus = status;
      return status;
    },
    artwork(id) {
      const handle = ops.artwork(trackId(id));
      return handle < 0 ? "pending" : handle;
    },
    releaseArtwork(handle) {
      if (handle > 0) ops.releaseArtwork(handle);
    },
  };
}
