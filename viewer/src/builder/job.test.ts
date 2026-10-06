import { describe, expect, it } from "vitest";
import { aboutTime, doneText, progressText, shortFile } from "./job";

describe("the job strip", () => {
  it("says about how long", () => {
    expect(aboutTime(0.4)).toBe("under a second");
    expect(aboutTime(42.4)).toBe("about 42 s");
    expect(aboutTime(200)).toBe("about 3 min");
  });

  it("counts cells and guesses the time left from the pace so far", () => {
    expect(progressText(0, 140, 0)).toBe("0 of 140 cells");
    expect(progressText(1, 140, 2)).toBe("1 of 140 cells"); // too early to guess
    expect(progressText(40, 140, 10)).toBe("40 of 140 cells · about 25 s left");
    expect(progressText(140, 140, 30)).toBe("140 of 140 cells");
  });

  it("says how a run ended and where it was saved", () => {
    expect(doneText({ cells: 140, cancelled: false, file: "out/matrix/x.json" }, 4.21)).toEqual({ text: "Done: 140 cells in 4.2 s", file: "out/matrix/x.json" });
    expect(doneText({ cells: 30, cancelled: true }, 2)).toEqual({ text: "Cancelled at 30 cells", file: null });
    expect(shortFile("out/matrix/20261006_clemson.json")).toBe("20261006_clemson.json");
  });
});
