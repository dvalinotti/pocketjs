/** Local encoded-media playback from the device's own storage. Control stays
 * on the guest; directory scan, tag parsing, decode and audio scheduling
 * belong to the native worker. Binary audio data never enters JS.
 *
 * Snapshot rule: every command updates the status snapshot before it
 * returns. open(id) reads back { trackId: id, openSerial: <its serial>,
 * phase: "loading", positionMs: 0 }; seek(ms) reads back the clamped
 * position, and an ended track becomes paused; paused(v) reads back
 * paused/playing. The worker never publishes state for a command a newer
 * command superseded.
 *
 * Open serial: every accepted open() returns a new serial, one greater than
 * the last, and every snapshot carries the serial of the open it describes.
 * A guest acts only on snapshots carrying the serial its latest open
 * returned, so a stale snapshot is ignored even when it names the same
 * track (repeat one).
 *
 * Artwork: artwork(id) never blocks. The first call for an id starts a decode
 * off the UI thread and returns -1 (pending); a later call returns the texture
 * handle once the decode finished, or 0 when the track has no art or it failed
 * to decode. One request is in flight: asking for another id abandons the
 * earlier one. A handle belongs to the guest until releaseArtwork(handle); a
 * call for the same id after its handle was handed out starts a new request. */
export const LOCALMEDIA = Object.freeze({
  version: 2,
  /** Scanned non-recursively for *.mp3 (extension case-insensitive). */
  root: "sdmc:/music/",
  maxTracks: 2048,
  /** Edge of the square artwork texture: the picture is centre-cropped to a square, then scaled. */
  artMax: 128,
});

export type LocalPhase = "idle" | "loading" | "playing" | "paused" | "ended" | "error";
const PHASES: ReadonlySet<string> = new Set<LocalPhase>(["idle", "loading", "playing", "paused", "ended", "error"]);

export interface LocalTrack {
  /** Stable for the session per file name: a rescan keeps a listed file's id, gives a new file the
   * next unused id, and gives a file that returns after vanishing its original id. Ids are never
   * reused for a different file. Ids are not positions; tracks() lists in scan order. */
  id: number;
  /** File name relative to LOCALMEDIA.root. */
  file: string;
  /** Tag text as UTF-8; the host applies the fallbacks (file stem, "Unknown Artist", "Unknown Album"). */
  title: string;
  artist: string;
  album: string;
  /** Track number; 0 when unknown. */
  track: number;
  /** Exact when the file has a Xing/Info/VBRI header; otherwise estimated from the first frame's
   * bitrate (exact for constant-bitrate files). 0 when unknown or the file does not decode. */
  durationMs: number;
  hasArt: boolean;
}

export interface LocalStatus {
  phase: LocalPhase;
  /** -1 before the first open. */
  trackId: number;
  /** Serial of the open this snapshot describes; 0 before the first. */
  openSerial: number;
  positionMs: number;
  durationMs: number;
  /** A scan is running; independent of the playback phase. */
  scanning: boolean;
  /** Completed lists; 0 before the first. A change means tracks() has a new list. A scan may complete
   * more than one: the list from the scan cache first (while `scanning` stays true), then the confirmed one. */
  scanGeneration: number;
  /** Milliseconds the last completed scan took to walk the folder; 0 before the first. */
  scanMs: number;
  underruns: number;
  error: string;
  /** Percent of real time the audio thread spent decoding over the last second (0..100). */
  decodeLoad: number;
  /** Artwork handles issued and not yet released. */
  artHandles: number;
}

export interface LocalMediaOps {
  /** Starts a scan of LOCALMEDIA.root; false while one is already running. */
  scan(): boolean;
  /** JSON LocalTrack[] of the last completed scan ("[]" before the first). */
  tracks(): string;
  /** Stops the current track and starts id. Returns the open's serial (> 0), or 0 for an id the last
   * completed scan did not list (a file removed since is refused; the track playing keeps playing). */
  open(id: number): number;
  paused(value: boolean): void;
  /** Milliseconds; the host clamps to [0, durationMs]. */
  seek(ms: number): void;
  /** 0..1. */
  volume(value: number): void;
  /** JSON LocalStatus. Non-blocking: no file, decoder or audio calls on the UI thread. */
  status(): string;
  /** -1 while pending; a texture handle (> 0) of artMax × artMax art once decoded; 0 when the track
   * has none, decoding failed, or the id is not in the last scan. Never blocks (see Artwork above). */
  artwork(id: number): number;
  releaseArtwork(handle: number): void;
}

const isInt = (value: unknown, min = 0): value is number => Number.isInteger(value) && (value as number) >= min;
const isObject = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null;

export function validLocalTrack(value: unknown): value is LocalTrack {
  return isObject(value) && isInt(value.id) && typeof value.file === "string"
    && typeof value.title === "string" && typeof value.artist === "string" && typeof value.album === "string"
    && isInt(value.track) && isInt(value.durationMs) && typeof value.hasArt === "boolean";
}

export function validLocalStatus(value: unknown): value is LocalStatus {
  return isObject(value) && typeof value.phase === "string" && PHASES.has(value.phase)
    && isInt(value.trackId, -1) && isInt(value.openSerial) && isInt(value.positionMs) && isInt(value.durationMs)
    && typeof value.scanning === "boolean" && isInt(value.scanGeneration) && isInt(value.scanMs)
    && isInt(value.underruns) && typeof value.error === "string"
    && isInt(value.decodeLoad) && isInt(value.artHandles);
}
