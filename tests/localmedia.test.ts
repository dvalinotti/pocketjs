import { expect, test } from "bun:test";
import { LOCALMEDIA, validLocalStatus, validLocalTrack, type LocalMediaOps, type LocalStatus } from "../contracts/spec/localmedia.ts";
import { POCKET_CAPABILITIES } from "../contracts/spec/platforms.ts";
import { localMedia } from "../framework/src/localmedia.ts";
import { resolve3dsBuildPlan } from "../tools/3ds-profile.ts";

const STATUS: LocalStatus = { phase: "playing", trackId: 1, openSerial: 4, positionMs: 10, durationMs: 100, scanning: false, scanGeneration: 1, scanMs: 900, underruns: 0, error: "", decodeLoad: 12, artHandles: 1, scratching: false };

function recorder(over: Partial<LocalMediaOps> = {}) {
  const calls: string[] = [];
  const ops: LocalMediaOps = {
    scan: () => (calls.push("scan"), true),
    tracks: () => "[]",
    open: (id) => (calls.push(`open ${id}`), 7),
    paused: (v) => void calls.push(`paused ${v}`),
    seek: (ms) => void calls.push(`seek ${ms}`),
    volume: (v) => void calls.push(`volume ${v}`),
    status: () => JSON.stringify(STATUS),
    artwork: (id) => (calls.push(`artwork ${id}`), 3),
    releaseArtwork: (h) => void calls.push(`release ${h}`),
    scratchBegin: () => void calls.push("scratchBegin"),
    scratchRate: (rate) => void calls.push(`scratchRate ${rate}`),
    scratchEnd: () => void calls.push("scratchEnd"),
    ...over,
  };
  return { calls, ops };
}

test("a realm without the namespace fails loudly", () => {
  expect(() => localMedia()).toThrow("Host does not implement media.local");
});

test("volume and seek are clamped before they cross; ids must be track ids", () => {
  const { calls, ops } = recorder();
  const media = localMedia(ops);
  media.volume(2); media.volume(-1); media.volume(NaN);
  media.seek(-5); media.seek(12.6); media.seek(Infinity);
  media.pause(true);
  expect(media.open(2)).toBe(7);
  expect(calls).toEqual(["volume 1", "volume 0", "volume 0", "seek 0", "seek 13", "seek 0", "paused true", "open 2"]);
  expect(() => media.open(-1)).toThrow("Invalid track id");
  expect(() => media.open(1.5)).toThrow("Invalid track id");
  expect(() => media.artwork(-2)).toThrow("Invalid track id");
});

test("scratch ops cross; the rate is clamped to ±maxScratchRate and a non-finite rate is 0", () => {
  const { calls, ops } = recorder();
  const media = localMedia(ops);
  media.scratchBegin();
  media.scratchRate(-1.5); media.scratchRate(9); media.scratchRate(-9); media.scratchRate(NaN); media.scratchRate(Infinity);
  media.scratchEnd();
  expect(calls).toEqual(["scratchBegin", "scratchRate -1.5", "scratchRate 4", "scratchRate -4", "scratchRate 0", "scratchRate 0", "scratchEnd"]);
  expect(localMedia(ops).status().scratching).toBe(false);
});

test("artwork -1 reads as pending; handles pass through", () => {
  let next = -1;
  const media = localMedia(recorder({ artwork: () => next }).ops);
  expect(media.artwork(4)).toBe("pending");
  next = 0;
  expect(media.artwork(4)).toBe(0);
  next = 9;
  expect(media.artwork(4)).toBe(9);
});

test("artwork handle 0 means none and is never released", () => {
  const { calls, ops } = recorder();
  const media = localMedia(ops);
  media.releaseArtwork(0);
  expect(calls).toEqual([]);
  media.releaseArtwork(3);
  expect(calls).toEqual(["release 3"]);
});

test("status and tracks are parsed and validated", () => {
  expect(localMedia(recorder().ops).status()).toEqual(STATUS);
  expect(() => localMedia(recorder({ status: () => JSON.stringify({ ...STATUS, phase: "scanning" }) }).ops).status()).toThrow("malformed status");
  expect(() => localMedia(recorder({ tracks: () => '[{"id":0}]' }).ops).tracks()).toThrow("malformed track list");
  expect(validLocalTrack({ id: 0, file: "a.mp3", title: "A", artist: "B", album: "C", track: 0, durationMs: 0, hasArt: false })).toBe(true);
  expect(validLocalStatus({ ...STATUS, trackId: -2 })).toBe(false);
  expect(validLocalStatus({ ...STATUS, trackId: -1 })).toBe(true);
  expect(validLocalStatus({ ...STATUS, openSerial: undefined })).toBe(false);
  expect(validLocalStatus({ ...STATUS, openSerial: -1 })).toBe(false);
  expect(validLocalStatus({ ...STATUS, decodeLoad: undefined })).toBe(false);
  expect(validLocalStatus({ ...STATUS, artHandles: -1 })).toBe(false);
  expect(validLocalStatus({ ...STATUS, scanMs: undefined })).toBe(false);
  expect(validLocalStatus({ ...STATUS, scanMs: -1 })).toBe(false);
  expect(validLocalStatus({ ...STATUS, scratching: undefined })).toBe(false);
  expect(LOCALMEDIA).toEqual({ version: 3, root: "sdmc:/music/", maxTracks: 2048, artMax: 128, maxScratchRate: 4 });
});

const probeManifest = (requires: string[]) => ({
  $schema: "https://pocketjs.dev/schema/pocket-2.json",
  pocket: 2, id: "dev.example.probe", name: "probe", title: "Probe", version: "0.1.0",
  engine: { capabilities: { requires: ["text.glyphs.baked", "input.buttons", "display.auxiliary", ...requires] } },
  app: {
    entry: "app/main.tsx", output: "probe-main", framework: "solid",
    viewport: { fixed: { logical: [400, 240], presentation: "native" } },
    surfaces: { auxiliary: { fixed: { logical: [320, 240], presentation: "native" } } },
  },
});

test("status() returns the same object while the host's reply is unchanged", () => {
  let reply = JSON.stringify(STATUS);
  const media = localMedia(recorder({ status: () => reply }).ops);
  const first = media.status();
  expect(media.status()).toBe(first);
  reply = JSON.stringify({ ...STATUS, positionMs: 20 });
  const second = media.status();
  expect(second).not.toBe(first);
  expect(second.positionMs).toBe(20);
  expect(media.status()).toBe(second);
  reply = "{"; // a bad reply is never served from the last good one
  expect(() => media.status()).toThrow();
});

test("the 3DS profile ships media.local", () => {
  expect(POCKET_CAPABILITIES).toContain("media.local");
  expect(resolve3dsBuildPlan(probeManifest(["media.local"])).features["media.local"]).toBe(true);
});

test("a 3DS app cannot declare both NDSP owners", () => {
  expect(() => resolve3dsBuildPlan(probeManifest(["media.local", "media.playback"]))).toThrow("media.playback and media.local both drive NDSP");
});
