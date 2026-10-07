// hosts/sim/localmedia.ts — the in-memory implementation of the local media
// module (contracts/spec/localmedia.ts) for the headless sim host.
//
// The library is a fixture list, not a directory: each entry declares its
// tags, duration, whether it carries art and whether its file decodes. Time
// passes only through advance(ms), so a run is a pure function of the
// fixture and the calls. Every command updates the snapshot before it
// returns (the contract's snapshot rule). Inject via bootWorld/bootBundle
// extraGlobals: { localmedia: host.ns }.

import { LOCALMEDIA, type LocalMediaOps, type LocalStatus, type LocalTrack } from "../../contracts/spec/localmedia.ts";

export interface SimLocalTrack {
  file: string;
  title?: string;
  artist?: string;
  album?: string;
  track?: number;
  durationMs: number;
  /** The file carries embedded art. */
  art?: boolean;
  /** The file does not decode: open() reaches "error" on the next advance. */
  corrupt?: boolean;
}

export interface SimLocalMediaOptions {
  /** Virtual time a scan takes. 0 (default) completes inside scan(). */
  scanMs?: number;
  /** Virtual time an artwork decode takes; the handle is ready on the first advance() at least
   * this long after the request. 0 (default): ready at the next advance(). */
  artworkMs?: number;
  /** A scan cache left by an earlier session: the first scan publishes it before the folder's list.
   * Later scans publish the previous scan's list first, as the native host does. */
  cached?: readonly SimLocalTrack[];
}

export interface SimLocalMediaHost {
  /** The `globalThis.localmedia` namespace. */
  ns: LocalMediaOps;
  /** Every op call except tracks()/status(), in order. */
  log: string[];
  volume(): number;
  /** Artwork handles issued and not yet released. */
  liveArtwork(): number[];
  /** The decodeLoad the status reports. */
  setDecodeLoad(percent: number): void;
  /** Move the virtual clock. */
  advance(ms: number): void;
  /** Replace what the "card" holds; the next scan() lists it (ids stay with their files). */
  setLibrary(next: readonly SimLocalTrack[]): void;
  dispose(): void;
}

const stem = (file: string) => file.replace(/\.[^.]*$/, "");

export function createSimLocalMedia(initial: readonly SimLocalTrack[], options: SimLocalMediaOptions = {}): SimLocalMediaHost {
  let library = initial;
  // Ids follow files for the life of the host: a rescan keeps a listed file's
  // id and gives a new file the next unused one.
  const idByFile = new Map<string, number>();
  let nextId = 0;
  const entryOf = new Map<number, SimLocalTrack>();
  const log: string[] = [];
  const art = new Set<number>();
  let nextArt = 1;
  /** The one artwork request in flight: its id, virtual age and whether time has passed since. */
  let artRequest: { id: number; age: number; advanced: boolean } | null = null;
  let volume = 1;
  let scanLeft = -1;
  let tracks: LocalTrack[] = [];
  let position = 0;
  let serial = 0;
  const status: LocalStatus = {
    phase: "idle", trackId: -1, openSerial: 0, positionMs: 0, durationMs: 0,
    scanning: false, scanGeneration: 0, scanMs: 0, underruns: 0, error: "", decodeLoad: 0, artHandles: 0,
  };
  /** What the scan cache holds: published first by the next scan. */
  let cache: readonly SimLocalTrack[] | null = options.cached ?? null;

  const publish = (list: readonly SimLocalTrack[]) => {
    entryOf.clear();
    tracks = list.slice(0, LOCALMEDIA.maxTracks).map((entry) => {
      let id = idByFile.get(entry.file);
      if (id === undefined) {
        id = nextId++;
        idByFile.set(entry.file, id);
      }
      entryOf.set(id, entry);
      return {
      id,
      file: entry.file,
      title: entry.title?.trim() || stem(entry.file),
      artist: entry.artist?.trim() || "Unknown Artist",
      album: entry.album?.trim() || "Unknown Album",
      track: entry.track ?? 0,
      durationMs: entry.corrupt ? 0 : entry.durationMs,
      hasArt: entry.art === true,
      };
    });
    status.scanGeneration++;
  };
  const finishScan = () => {
    publish(library);
    cache = library;
    scanLeft = -1;
    status.scanning = false;
    status.scanMs = Math.max(0, options.scanMs ?? 0);
  };
  const setPosition = (ms: number) => {
    position = ms;
    status.positionMs = Math.floor(ms);
  };

  const ns: LocalMediaOps = {
    scan() {
      log.push("scan()");
      if (status.scanning) return false;
      status.scanning = true;
      if (cache) publish(cache);
      const scanMs = options.scanMs ?? 0;
      if (scanMs <= 0) finishScan();
      else scanLeft = scanMs;
      return true;
    },
    tracks: () => JSON.stringify(tracks),
    open(id) {
      log.push(`open(${id})`);
      const track = tracks.find((t) => t.id === id);
      if (!track) return 0;
      serial++;
      Object.assign(status, { phase: "loading", trackId: id, openSerial: serial, durationMs: track.durationMs, error: "" });
      setPosition(0);
      return serial;
    },
    paused(value) {
      log.push(`paused(${value})`);
      if (value && (status.phase === "playing" || status.phase === "loading")) status.phase = "paused";
      else if (!value && status.phase === "paused") status.phase = "playing";
    },
    seek(ms) {
      log.push(`seek(${ms})`);
      if (status.trackId < 0 || status.phase === "idle" || status.phase === "error") return;
      setPosition(Math.min(Math.max(0, ms), status.durationMs));
      if (status.phase === "ended") status.phase = "paused";
    },
    volume(value) {
      log.push(`volume(${value})`);
      volume = value;
    },
    status: () => JSON.stringify(status),
    artwork(id) {
      log.push(`artwork(${id})`);
      if (!tracks.find((t) => t.id === id)?.hasArt) return 0;
      if (artRequest?.id !== id) {
        artRequest = { id, age: 0, advanced: false };
        return -1;
      }
      if (!artRequest.advanced || artRequest.age < (options.artworkMs ?? 0)) return -1;
      artRequest = null;
      const handle = nextArt++;
      art.add(handle);
      status.artHandles = art.size;
      return handle;
    },
    releaseArtwork(handle) {
      log.push(`releaseArtwork(${handle})`);
      art.delete(handle);
      status.artHandles = art.size;
    },
  };

  return {
    ns,
    log,
    volume: () => volume,
    liveArtwork: () => [...art],
    setDecodeLoad(percent) {
      status.decodeLoad = percent;
    },
    advance(ms) {
      if (artRequest) {
        artRequest.age += ms;
        artRequest.advanced = true;
      }
      if (scanLeft >= 0) {
        scanLeft -= ms;
        if (scanLeft <= 0) finishScan();
      }
      if (status.phase === "loading") {
        if (entryOf.get(status.trackId)?.corrupt) Object.assign(status, { phase: "error", error: "MP3 frame sync not found" });
        else status.phase = "playing";
        return;
      }
      if (status.phase !== "playing") return;
      if (position + ms >= status.durationMs) {
        setPosition(status.durationMs);
        status.phase = "ended";
      } else setPosition(position + ms);
    },
    setLibrary(next) {
      library = next;
    },
    dispose() {
      art.clear();
      artRequest = null;
      tracks = [];
      log.length = 0;
    },
  };
}
