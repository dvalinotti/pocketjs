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

test("artwork handles exist only for tracks with art and stay live until released", () => {
  const { host, media } = setup();
  media.scan();
  const handle = media.artwork(0);
  expect(handle).toBeGreaterThan(0);
  expect(media.artwork(1)).toBe(0);
  expect(host.liveArtwork()).toEqual([handle]);
  media.releaseArtwork(handle);
  expect(host.liveArtwork()).toEqual([]);
  media.volume(0.25);
  expect(host.volume()).toBe(0.25);
  expect(host.log).toEqual(["scan()", "artwork(0)", "artwork(1)", `releaseArtwork(${handle})`, "volume(0.25)"]);
});

test("ids stay with their files across a rescan; new files get fresh ids; a vanished file does not open", () => {
  const host = createSimLocalMedia(LIB);
  const media = localMedia(host.ns);
  media.scan();
  expect(media.tracks().map((t) => [t.id, t.file])).toEqual([[0, "01 Intro.mp3"], [1, "untagged.mp3"], [2, "broken.mp3"]]);
  host.setLibrary([{ file: "00 New.mp3", durationMs: 500 }, LIB[1]!, LIB[0]!]);
  media.scan();
  expect(media.status().scanGeneration).toBe(2);
  expect(media.tracks().map((t) => [t.id, t.file])).toEqual([[3, "00 New.mp3"], [1, "untagged.mp3"], [0, "01 Intro.mp3"]]);
  expect(media.open(2)).toBe(0);
  expect(media.open(3)).toBeGreaterThan(0);
  expect(media.status()).toMatchObject({ trackId: 3, durationMs: 500 });
  expect(media.artwork(0)).toBeGreaterThan(0);
});
