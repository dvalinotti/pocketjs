import { expect, test } from "bun:test";
import { createSimLocalMedia, type SimLocalTrack } from "../hosts/sim/localmedia.ts";
import { localMedia } from "../framework/src/localmedia.ts";

const LIB: SimLocalTrack[] = [
  { file: "01 Intro.mp3", title: "Intro", artist: "Band", album: "First", track: 1, durationMs: 1000, art: true },
  { file: "untagged.mp3", title: "  ", artist: "", durationMs: 2000 },
  { file: "broken.mp3", title: "Broken", durationMs: 3000, corrupt: true },
];

function setup(options = {}) {
  const host = createSimLocalMedia(LIB, options);
  return { host, media: localMedia(host.ns) };
}

test("a scan lists every fixture with the native tag fallbacks, blank tags included", () => {
  const { media } = setup();
  expect(media.tracks()).toEqual([]);
  expect(media.scan()).toBe(true);
  expect(media.status()).toMatchObject({ scanning: false, scanGeneration: 1 });
  expect(media.tracks()).toEqual([
    { id: 0, file: "01 Intro.mp3", title: "Intro", artist: "Band", album: "First", track: 1, durationMs: 1000, hasArt: true },
    { id: 1, file: "untagged.mp3", title: "untagged", artist: "Unknown Artist", album: "Unknown Album", track: 0, durationMs: 2000, hasArt: false },
    { id: 2, file: "broken.mp3", title: "Broken", artist: "Unknown Artist", album: "Unknown Album", track: 0, durationMs: 0, hasArt: false },
  ]);
});

test("a timed scan reports scanning until its virtual time passes", () => {
  const { host, media } = setup({ scanMs: 500 });
  media.scan();
  expect(media.status()).toMatchObject({ scanning: true, scanGeneration: 0 });
  expect(media.scan()).toBe(false);
  host.advance(499);
  expect(media.status().scanning).toBe(true);
  host.advance(1);
  expect(media.status()).toMatchObject({ scanning: false, scanGeneration: 1 });
  expect(media.tracks()).toHaveLength(3);
});

test("every command is visible in the next status read", () => {
  const { host, media } = setup();
  media.scan();
  expect(media.status().openSerial).toBe(0);
  expect(media.open(0)).toBe(1);
  expect(media.status()).toMatchObject({ phase: "loading", trackId: 0, openSerial: 1, positionMs: 0, durationMs: 1000 });
  host.advance(16);
  expect(media.status()).toMatchObject({ phase: "playing", positionMs: 0 });
  host.advance(400);
  expect(media.status().positionMs).toBe(400);
  media.pause(true);
  expect(media.status().phase).toBe("paused");
  host.advance(400);
  expect(media.status().positionMs).toBe(400);
  media.pause(false);
  media.seek(5000);
  expect(media.status()).toMatchObject({ phase: "playing", positionMs: 1000 });
  host.advance(1);
  expect(media.status()).toMatchObject({ phase: "ended", positionMs: 1000 });
  expect(media.open(0)).toBe(2);
  expect(media.status()).toMatchObject({ phase: "loading", trackId: 0, openSerial: 2, positionMs: 0 });
});

test("a seek on an ended track pauses it at the target", () => {
  const { host, media } = setup();
  media.scan();
  media.open(0);
  host.advance(16);
  host.advance(1000);
  expect(media.status().phase).toBe("ended");
  media.seek(0);
  expect(media.status()).toMatchObject({ phase: "paused", positionMs: 0 });
});

test("an undecodable file reaches error after loading; an unscanned id does not open", () => {
  const { host, media } = setup();
  media.scan();
  expect(media.open(7)).toBe(0);
  expect(media.status()).toMatchObject({ trackId: -1, openSerial: 0 });
  media.open(2);
  host.advance(16);
  expect(media.status()).toMatchObject({ phase: "error", trackId: 2, error: "MP3 frame sync not found" });
  media.seek(10);
  expect(media.status().positionMs).toBe(0);
});

test("artwork is pending until time passes, exists only for tracks with art, and stays live until released", () => {
  const { host, media } = setup();
  media.scan();
  expect(media.artwork(0)).toBe("pending");
  expect(media.artwork(0)).toBe("pending");
  expect(media.artwork(1)).toBe(0);
  host.advance(16);
  const handle = media.artwork(0);
  expect(handle).toBeGreaterThan(0);
  expect(host.liveArtwork()).toEqual([handle as number]);
  expect(media.status().artHandles).toBe(1);
  media.releaseArtwork(handle as number);
  expect(host.liveArtwork()).toEqual([]);
  expect(media.status().artHandles).toBe(0);
  media.volume(0.25);
  expect(host.volume()).toBe(0.25);
  expect(host.log).toEqual(["scan()", "artwork(0)", "artwork(0)", "artwork(1)", "artwork(0)", `releaseArtwork(${handle})`, "volume(0.25)"]);
});

test("a slow artwork decode stays pending for its virtual time; asking for another id abandons it", () => {
  const { host, media } = setup({ artworkMs: 100 });
  media.scan();
  host.setLibrary([LIB[0]!, { ...LIB[1]!, art: true }]);
  media.scan();
  expect(media.artwork(0)).toBe("pending");
  host.advance(60);
  expect(media.artwork(0)).toBe("pending");
  expect(media.artwork(1)).toBe("pending"); // abandons 0
  host.advance(60);
  expect(media.artwork(0)).toBe("pending"); // a fresh request
  host.advance(100);
  expect(media.artwork(0)).toBeGreaterThan(0);
  expect(media.artwork(0)).toBe("pending"); // after hand-out, a new request
  host.setDecodeLoad(37);
  expect(media.status().decodeLoad).toBe(37);
});

test("a file that vanishes and returns gets its original id back", () => {
  const host = createSimLocalMedia(LIB);
  const media = localMedia(host.ns);
  media.scan();
  host.setLibrary([LIB[1]!]);
  media.scan();
  host.setLibrary([{ file: "new.mp3", durationMs: 10 }, LIB[0]!, LIB[1]!]);
  media.scan();
  expect(media.tracks().map((t) => [t.id, t.file])).toEqual([[3, "new.mp3"], [0, "01 Intro.mp3"], [1, "untagged.mp3"]]);
});

test("ids stay with their files across a rescan; new files get fresh ids; a vanished file does not open", () => {
  const host = createSimLocalMedia(LIB);
  const media = localMedia(host.ns);
  media.scan();
  expect(media.tracks().map((t) => [t.id, t.file])).toEqual([[0, "01 Intro.mp3"], [1, "untagged.mp3"], [2, "broken.mp3"]]);
  host.setLibrary([{ file: "00 New.mp3", durationMs: 500 }, LIB[1]!, LIB[0]!]);
  media.scan();
  expect(media.status().scanGeneration).toBe(2); // a rescan reads the folder: the cache serves only the first scan
  expect(media.tracks().map((t) => [t.id, t.file])).toEqual([[3, "00 New.mp3"], [1, "untagged.mp3"], [0, "01 Intro.mp3"]]);
  expect(media.open(2)).toBe(0);
  expect(media.open(3)).toBeGreaterThan(0);
  expect(media.status()).toMatchObject({ trackId: 3, durationMs: 500 });
  expect(media.artwork(0)).toBe("pending");
  host.advance(1);
  expect(media.artwork(0)).toBeGreaterThan(0);
});

test("a scan publishes the cached list first, keeps scanning, then publishes the folder's list", () => {
  const host = createSimLocalMedia([LIB[0]!, LIB[1]!], { scanMs: 500, cached: [LIB[1]!] });
  const media = localMedia(host.ns);
  media.scan();
  expect(media.status()).toMatchObject({ scanning: true, scanGeneration: 1, scanMs: 0 });
  expect(media.tracks().map((t) => t.file)).toEqual(["untagged.mp3"]);
  host.advance(500);
  expect(media.status()).toMatchObject({ scanning: false, scanGeneration: 2, scanMs: 500 });
  expect(media.tracks().map((t) => t.file)).toEqual(["01 Intro.mp3", "untagged.mp3"]);
  expect(media.tracks().find((t) => t.file === "untagged.mp3")!.id).toBe(0); // ids follow files across both lists
});

test("an empty cache publishes nothing: the first scan stays in progress until the folder's list", () => {
  const host = createSimLocalMedia([LIB[0]!], { scanMs: 500, cached: [] });
  const media = localMedia(host.ns);
  media.scan();
  expect(media.status()).toMatchObject({ scanning: true, scanGeneration: 0 });
  host.advance(500);
  expect(media.status()).toMatchObject({ scanning: false, scanGeneration: 1 });
  expect(media.tracks().map((t) => t.file)).toEqual(["01 Intro.mp3"]);
});

test("scratching moves the position by rate × time either way, within 10 s behind the grab, paused too, and never ends", () => {
  const { host, media } = setup();
  media.scan();
  media.open(0); // 1000 ms
  host.advance(1);
  host.advance(400);
  expect(media.status()).toMatchObject({ phase: "playing", positionMs: 400, scratching: false });
  media.scratchBegin();
  expect(media.status().scratching).toBe(true);
  host.advance(100); // rate 0: still
  expect(media.status().positionMs).toBe(400);
  media.scratchRate(-1);
  host.advance(100);
  expect(media.status().positionMs).toBe(300);
  media.scratchRate(-4);
  host.advance(1000); // clamped at 0 (the start is within 10 s)
  expect(media.status().positionMs).toBe(0);
  media.scratchRate(4);
  host.advance(1000); // clamped at the duration, and not ended
  expect(media.status()).toMatchObject({ positionMs: 1000, phase: "playing", scratching: true });
  media.scratchEnd();
  expect(media.status().scratching).toBe(false);
  host.advance(1);
  expect(media.status().phase).toBe("ended");
  expect(host.log.filter((entry) => entry.startsWith("scratch"))).toEqual(["scratchBegin()", "scratchRate(-1)", "scratchRate(-4)", "scratchRate(4)", "scratchEnd()"]);
});

test("a grab is ignored unless playing or paused; paused scratching moves and stays paused; open and seek end it", () => {
  const { host, media } = setup();
  media.scan();
  media.scratchBegin(); // idle
  expect(media.status().scratching).toBe(false);
  media.open(1); // 2000 ms
  media.scratchBegin(); // loading
  expect(media.status().scratching).toBe(false);
  host.advance(1);
  media.pause(true);
  media.scratchBegin();
  media.scratchRate(1);
  host.advance(250);
  expect(media.status()).toMatchObject({ phase: "paused", scratching: true, positionMs: 250 });
  media.scratchEnd();
  host.advance(250);
  expect(media.status()).toMatchObject({ phase: "paused", positionMs: 250 });
  media.pause(false);
  media.scratchBegin();
  media.seek(1000);
  expect(media.status().scratching).toBe(false);
  media.scratchBegin();
  media.open(0);
  expect(media.status().scratching).toBe(false);
});

test("a long reverse scratch holds 10 s behind the grab", () => {
  const host = createSimLocalMedia([{ file: "long.mp3", durationMs: 60_000 }]);
  const media = localMedia(host.ns);
  media.scan();
  media.open(0);
  host.advance(1);
  host.advance(30_000);
  media.scratchBegin();
  media.scratchRate(-4);
  host.advance(5_000);
  expect(media.status().positionMs).toBe(20_000);
});
