import { afterAll, describe, expect, test } from "bun:test";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { validLocalTrack, type LocalTrack } from "../contracts/spec/localmedia.ts";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "..");
const FIXTURES = join(ROOT, "tests/fixtures/localmedia");
const SRC = join(ROOT, "hosts/3ds/src");
const scratch = mkdtempSync(join(tmpdir(), "pocket-localmedia-"));
afterAll(() => rmSync(scratch, { recursive: true, force: true }));

/** Compiles a harness with the given media.local sources under ASan/UBSan and runs it from the fixtures folder. */
function run(harness: string, sources: string[], args: string[] = [], extra: string[] = []): string {
  const binary = join(scratch, harness.replace(/\.c$/, ""));
  const compile = Bun.spawnSync(["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
    `-I${SRC}`, `-I${join(ROOT, "hosts/3ds/vendor")}`, join(FIXTURES, harness), ...sources.map((s) => join(SRC, s)), ...extra, "-o", binary]);
  if (compile.exitCode !== 0) throw new Error(`compile ${harness} failed:\n${compile.stderr.toString()}`);
  const result = Bun.spawnSync([binary, ...args], { cwd: FIXTURES, timeout: 60_000 });
  if (result.exitCode !== 0) throw new Error(`${harness} failed (exit ${result.exitCode}):\n${result.stderr.toString()}${result.stdout.toString()}`);
  return result.stdout.toString();
}

describe("media.local native units (host-compiled)", () => {
  test("ids stay with their files: kept, added, vanished, returning, never reused", () => {
    expect(run("ids-test.c", ["localmedia_ids.c"])).toContain("localmedia ids verified");
  }, 60_000);

  test("ring: writes, wraps, converts channels, copies, resamples forward/backward/fractional, ramps, clamps", () => {
    expect(run("ring-test.c", ["localmedia_ring.c"])).toContain("localmedia ring verified");
  }, 60_000);

  test("tags: v2.2/2.3/2.4 fields, encodings, unsync, pictures, v1 fallback, broken input", () => {
    expect(run("tags-test.c", ["localmedia_tags.c"])).toContain("localmedia tags verified");
  }, 60_000);

  test("mp3: frame table, Xing/Info/VBRI durations, estimates, TOC and linear seek, resync", () => {
    expect(run("mp3-test.c", ["localmedia_mp3.c"])).toContain("localmedia mp3 verified");
  }, 60_000);

  test("art: crop and box scale, embedded JPEG and PNG, limits, unsync reads", () => {
    expect(run("art-test.c", ["localmedia_art.c", "localmedia_tags.c"])).toContain("localmedia art verified");
  }, 60_000);

  test("cache: round trip, lookups by name and size, damaged files read as empty, atomic rewrite", () => {
    expect(run("cache-test.c", ["localmedia_cache.c"])).toContain("localmedia cache verified");
  }, 60_000);

  test("scan: cache reuse, changed/new/removed files, same JSON, two readers, every failed allocation", () => {
    expect(run("scan-cache-test.c", ["localmedia_library.c", "localmedia_dir.c", "localmedia_cache.c", "localmedia_tags.c", "localmedia_mp3.c", "localmedia_ids.c"], [],
      ["-DLM_TEST_ALLOC_FAIL", join(FIXTURES, "alloc-fail.c")])).toContain("localmedia scan cache verified");
  }, 60_000);

  test("library: lists *.mp3 only, applies fallbacks, keeps ids across rescans, caps the count", () => {
    const folder = (name: string, files: Record<string, string>) => {
      const dir = join(scratch, name);
      mkdirSync(dir, { recursive: true });
      for (const [file, from] of Object.entries(files)) {
        if (from === "<dir>") mkdirSync(join(dir, file));
        else if (from === "<junk>") writeFileSync(join(dir, file), Buffer.alloc(3000, 0x11));
        else copyFileSync(join(FIXTURES, from), join(dir, file));
      }
      return `${dir}/`;
    };
    const first = folder("scan-1", {
      "tagged-v23.mp3": "tagged-v23.mp3", "LOUD.MP3": "cbr-info.mp3", "plain \"quoted\".mp3": "cbr-plain.mp3",
      "junk.mp3": "<junk>", "notes.txt": "cover-small.png", "folder.mp3": "<dir>", "v1.Mp3": "tagged-v1.mp3",
    });
    const second = folder("scan-2", { "tagged-v23.mp3": "tagged-v23.mp3", "new.mp3": "vbr-xing.mp3", "v1.Mp3": "tagged-v1.mp3" });
    const third = folder("scan-3", { "LOUD.MP3": "cbr-info.mp3", "new.mp3": "vbr-xing.mp3" });
    const capped = folder("scan-4", { "a.mp3": "cbr-info.mp3", "b.mp3": "cbr-info.mp3", "c.mp3": "cbr-info.mp3" });
    const missing = join(scratch, "no-such-folder/");
    const lines = run("library-test.c", ["localmedia_library.c", "localmedia_dir.c", "localmedia_cache.c", "localmedia_tags.c", "localmedia_mp3.c", "localmedia_ids.c"],
      ["2048", first, second, third, missing, capped]).trim().split("\n");
    const scans = lines.map((line) => JSON.parse(line) as LocalTrack[]);
    for (const scan of scans) expect(scan.every(validLocalTrack)).toBe(true);
    const byFile = (scan: LocalTrack[]) => Object.fromEntries(scan.map((t) => [t.file, t]));
    const one = byFile(scans[0]!);
    expect(Object.keys(one).sort()).toEqual(["LOUD.MP3", "junk.mp3", "plain \"quoted\".mp3", "tagged-v23.mp3", "v1.Mp3"]);
    expect(one["tagged-v23.mp3"]).toMatchObject({ title: "Café", artist: "Björk", album: "Début", track: 3, durationMs: 1044, hasArt: true });
    expect(one["LOUD.MP3"]).toMatchObject({ title: "LOUD", artist: "Unknown Artist", album: "Unknown Album", track: 0, durationMs: 1044, hasArt: false });
    expect(one["plain \"quoted\".mp3"]!.title).toBe('plain "quoted"');
    expect(one["junk.mp3"]).toMatchObject({ title: "junk", durationMs: 0, hasArt: false });
    expect(one["v1.Mp3"]).toMatchObject({ title: "Old Tag", artist: "V1 Artist", album: "V1 Album", track: 5 });
    expect(new Set(scans[0]!.map((t) => t.id)).size).toBe(5);
    const two = byFile(scans[1]!), three = byFile(scans[2]!);
    expect(two["tagged-v23.mp3"]!.id).toBe(one["tagged-v23.mp3"]!.id);
    expect(two["v1.Mp3"]!.id).toBe(one["v1.Mp3"]!.id);
    expect(two["new.mp3"]!.id).toBe(5);
    expect(three["LOUD.MP3"]!.id).toBe(one["LOUD.MP3"]!.id); // vanished in scan 2, back with its id
    expect(three["new.mp3"]!.id).toBe(5);
    expect(scans[3]).toEqual([]);
    expect(scans[4]!.map((t) => t.id).sort()).toEqual([6, 7, 8]);
    const cappedLines = run("library-test.c", ["localmedia_library.c", "localmedia_dir.c", "localmedia_cache.c", "localmedia_tags.c", "localmedia_mp3.c", "localmedia_ids.c"], ["2", capped]);
    expect((JSON.parse(cappedLines.trim()) as LocalTrack[]).length).toBe(2);
  }, 60_000);

  test("player: decodes, positions, seeks, ends, counts underruns, reports unreadable files", () => {
    expect(run("player-test.c", ["localmedia_player.c", "localmedia_ring.c", "localmedia_mp3.c", "localmedia_tags.c"])).toContain("localmedia player verified");
  }, 60_000);

  test("glue: snapshots, failed opens, end-of-track waiting, underruns, art, long names, superseded opens, decode load, art during scans, out of memory", () => {
    // A long folder path, so a long name overflows any 300-byte path buffer.
    const music = join(scratch, `glue-music-${"x".repeat(60)}`);
    mkdirSync(music, { recursive: true });
    for (const [file, from] of [["a.mp3", "cbr-info.mp3"], ["b.mp3", "cbr-plain.mp3"], ["art.mp3", "tagged-v23.mp3"], [`${"é".repeat(123)}.mp3`, "cbr-info.mp3"]]) copyFileSync(join(FIXTURES, from!), join(music, file!));
    for (let i = 0; i < 300; i++) copyFileSync(join(FIXTURES, "cbr-plain.mp3"), join(music, `pad-${String(i).padStart(3, "0")}.mp3`));
    writeFileSync(join(music, "short.mp3"), readFileSync(join(FIXTURES, "cbr-plain.mp3")).subarray(0, 5000));
    writeFileSync(join(music, "junk.mp3"), Buffer.alloc(70_000, 0x11));
    const glue = join(FIXTURES, "glue");
    const binary = join(scratch, "glue-test");
    const units = ["localmedia.c", "localmedia_cache.c", "localmedia_dir.c", "localmedia_ids.c", "localmedia_tags.c", "localmedia_mp3.c", "localmedia_art.c", "localmedia_library.c", "localmedia_player.c", "localmedia_ring.c"];
    const compile = Bun.spawnSync(["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
      "-DLM_TEST_ALLOC_FAIL", `-DLOCALMEDIA_ROOT="${music}/"`, `-DLOCALMEDIA_CACHE_DIR="${join(scratch, "glue-cache")}"`, `-I${glue}`, `-I${join(ROOT, "hosts/3ds/include")}`, `-I${SRC}`, `-I${join(ROOT, "hosts/3ds/vendor")}`,
      join(glue, "glue-test.c"), join(glue, "glue-fake.c"), join(FIXTURES, "alloc-fail.c"), ...units.map((unit) => join(SRC, unit)), "-o", binary]);
    if (compile.exitCode !== 0) throw new Error(`compile glue-test.c failed:\n${compile.stderr.toString()}`);
    const result = Bun.spawnSync([binary], { cwd: FIXTURES, timeout: 60_000 });
    if (result.exitCode !== 0) throw new Error(`glue-test failed (exit ${result.exitCode}):\n${result.stderr.toString()}${result.stdout.toString()}`);
    expect(result.stdout.toString()).toContain("localmedia glue verified");
  }, 90_000);
});
