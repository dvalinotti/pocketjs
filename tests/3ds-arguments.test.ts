import { expect, test } from "bun:test";
import { parse3dsArguments } from "../tools/3ds.ts";

test("font overrides and extra characters reach the guest build, resolved against the working directory", () => {
  const args = parse3dsArguments(
    ["--font-regular=fonts/W95FA.otf", "--font-bold=/abs/W95FA-Bold.otf", "--extra-chars=♪▶"],
    { workingDirectory: "/work/app" },
  );
  expect(args.buildFlags).toEqual([
    "--font-regular=/work/app/fonts/W95FA.otf",
    "--font-bold=/abs/W95FA-Bold.otf",
    "--extra-chars=♪▶",
  ]);
  expect(args.cargoArgs).toEqual([]);
});

test("unrecognized flags still go to cargo", () => {
  expect(parse3dsArguments(["--locked"], { workingDirectory: "/w" }).cargoArgs).toEqual(["--locked"]);
});
