import { afterAll, describe, expect, test } from "bun:test";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "..");
const FIXTURES = join(ROOT, "tests/fixtures/localmedia");
const SRC = join(ROOT, "hosts/3ds/src");
const scratch = mkdtempSync(join(tmpdir(), "pocket-localmedia-"));
afterAll(() => rmSync(scratch, { recursive: true, force: true }));

/** Compiles a harness with the given media.local sources under ASan/UBSan and runs it from the fixtures folder. */
function run(harness: string, sources: string[]): string {
  const binary = join(scratch, harness.replace(/\.c$/, ""));
  const compile = Bun.spawnSync(["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
    `-I${SRC}`, `-I${join(ROOT, "hosts/3ds/vendor")}`, join(FIXTURES, harness), ...sources.map((s) => join(SRC, s)), "-o", binary]);
  if (compile.exitCode !== 0) throw new Error(`compile ${harness} failed:\n${compile.stderr.toString()}`);
  const result = Bun.spawnSync([binary], { cwd: FIXTURES, timeout: 60_000 });
  if (result.exitCode !== 0) throw new Error(`${harness} failed (exit ${result.exitCode}):\n${result.stderr.toString()}${result.stdout.toString()}`);
  return result.stdout.toString();
}

describe("media.local native units (host-compiled)", () => {
  test("ids stay with their files: kept, added, vanished, returning, never reused", () => {
    expect(run("ids-test.c", ["localmedia_ids.c"])).toContain("localmedia ids verified");
  }, 60_000);

  test("tags: v2.2/2.3/2.4 fields, encodings, unsync, pictures, v1 fallback, broken input", () => {
    expect(run("tags-test.c", ["localmedia_tags.c"])).toContain("localmedia tags verified");
  }, 60_000);

  test("mp3: frame table, Xing/Info/VBRI durations, estimates, TOC and linear seek, resync", () => {
    expect(run("mp3-test.c", ["localmedia_mp3.c"])).toContain("localmedia mp3 verified");
  }, 60_000);
});
