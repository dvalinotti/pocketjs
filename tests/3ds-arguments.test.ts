import { expect, test } from "bun:test";
import { parse3dsArguments } from "../tools/3ds.ts";

test("unrecognized flags still go to cargo", () => {
  expect(parse3dsArguments(["--locked"], { workingDirectory: "/w" }).cargoArgs).toEqual(["--locked"]);
});

test("application configuration flags are refused in favour of pocket.json", () => {
  expect(() => parse3dsArguments(["--config=app.json"], { workingDirectory: "/w" })).toThrow(/belongs in pocket.json/);
});
