import { expect, test } from "bun:test";
import { LOCALMEDIA, validLocalStatus, validLocalTrack, type LocalMediaOps } from "../contracts/spec/localmedia.ts";
import { POCKET_CAPABILITIES } from "../contracts/spec/platforms.ts";
import { localMedia } from "../framework/src/localmedia.ts";
import { resolve3dsBuildPlan } from "../tools/3ds-profile.ts";

const STATUS = { phase: "playing", trackId: 1, positionMs: 10, durationMs: 100, scanning: false, scanGeneration: 1, underruns: 0, error: "" };

function recorder(over: Partial<LocalMediaOps> = {}) {
  const calls: string[] = [];
  const ops: LocalMediaOps = {
    scan: () => (calls.push("scan"), true),
    tracks: () => "[]",
    open: (id) => (calls.push(`open ${id}`), true),
    paused: (v) => void calls.push(`paused ${v}`),
    seek: (ms) => void calls.push(`seek ${ms}`),
    volume: (v) => void calls.push(`volume ${v}`),
    status: () => JSON.stringify(STATUS),
    artwork: (id) => (calls.push(`artwork ${id}`), 3),
    releaseArtwork: (h) => void calls.push(`release ${h}`),
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
  expect(calls).toEqual(["volume 1", "volume 0", "volume 0", "seek 0", "seek 13", "seek 0", "paused true"]);
  expect(() => media.open(-1)).toThrow("Invalid track id");
  expect(() => media.open(1.5)).toThrow("Invalid track id");
  expect(() => media.artwork(-2)).toThrow("Invalid track id");
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
  expect(LOCALMEDIA).toEqual({ version: 1, root: "sdmc:/music/", maxTracks: 2048, artMax: 128 });
});

test("media.local is a registered capability that the 3DS profile does not advertise yet", () => {
  expect(POCKET_CAPABILITIES).toContain("media.local");
  const plan = resolve3dsBuildPlan({
    $schema: "https://pocketjs.dev/schema/pocket-2.json",
    pocket: 2, id: "dev.example.probe", name: "probe", title: "Probe", version: "0.1.0",
    engine: { capabilities: { requires: ["text.glyphs.baked", "input.buttons", "display.auxiliary"], enhances: ["media.local"] } },
    app: {
      entry: "app/main.tsx", output: "probe-main", framework: "solid",
      viewport: { fixed: { logical: [400, 240], presentation: "native" } },
      surfaces: { auxiliary: { fixed: { logical: [320, 240], presentation: "native" } } },
    },
  });
  expect(plan.features["media.local"]).toBe(false);
});
