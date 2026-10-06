// The detail card for one cell: everything the server counted for it, as
// labelled rows. Pure, so the numbers are tested.

import { appliesTo, formatValue, halfWidth, metricsFor, reading, type CellStats, type Domain, type KickStats, type PlayKind, type Stats } from "./results";

export interface Section {
  title: string;
  rows: [label: string, value: string][];
}

const f = (v: unknown, digits = 1): string => (typeof v === "number" && Number.isFinite(v) ? v.toFixed(digits) : "–");
const pct = (num: number, den: number, digits = 0): string => (den > 0 ? `${((100 * num) / den).toFixed(digits)}%` : "–");
const per = (num: number, den: number): string => (den > 0 ? (num / den).toFixed(2) : "–");

interface Spread {
  count: number;
  mean: number;
  p10: number;
  p50: number;
  p90: number;
  share_nonpositive: number;
  share_10_plus: number;
  share_20_plus: number;
}

// The headline numbers: every metric that means something for this kind of row.
function results(s: Stats, kind: PlayKind, domain: Domain, title: string): Section {
  return {
    title,
    rows: metricsFor(domain).filter((m) => appliesTo(m, kind)).map((m) => {
      const r = reading(m, s, kind);
      const hw = halfWidth(r);
      const range = hw === null ? "" : ` ± ${formatValue(m, hw)}`;
      return [m.label, r ? formatValue(m, r.value) + range : "–"];
    }),
  };
}

// What the kicks added up to: how each ended, and the averages.
export function kickSections(s: KickStats, kind: PlayKind): Section[] {
  const out: Section[] = [results(s, kind, "kicks", `Results (${s.kicks} kicks)`)];
  const names = Object.entries((s.results ?? {}) as Record<string, number>).sort((a, b) => b[1] - a[1]);
  if (names.length) out.push({ title: "How the kicks ended", rows: names.map(([name, n]) => [name.replace(/_/g, " "), `${n} (${pct(n, s.kicks, 0)})`]) });
  if (kind !== "field_goal") {
    out.push({
      title: "Averages",
      rows: [
        ["Kick distance", `${f(s.distance / s.kicks)} yd`],
        ["Hang time", `${f(s.hang / s.kicks, 2)} s`],
        ["Returns", `${s.returns} (${f(s.returns > 0 ? s.return_yards / s.returns : null)} yd each)`],
        ["Net", `${f(s.net_yards / s.kicks)} yd`],
        ["Play length", `${f(Number(s.seconds) / s.kicks)} s`],
      ],
    });
  }
  return out;
}

export function detailSections(stats: Stats, kind: PlayKind, domain: Domain = "scrimmage"): Section[] {
  if (domain === "kicks") return kickSections(stats as KickStats, kind);
  const s = stats as CellStats;
  const out: Section[] = [results(s, kind, "scrimmage", `Results (${s.plays} plays)`)];

  const g = s.gains as Spread | undefined;
  if (g && g.count > 0) {
    out.push({
      title: `Yards of the ${g.count} plays that gained`,
      rows: [
        ["Mean", f(g.mean)],
        ["10 / 50 / 90%", `${f(g.p10)}  ${f(g.p50)}  ${f(g.p90)}`],
        ["0 or less", `${f(g.share_nonpositive, 0)}%`],
        ["10+", `${f(g.share_10_plus, 0)}%`],
        ["20+", `${f(g.share_20_plus, 0)}%`],
      ],
    });
  }

  if (kind === "pass") {
    const pocket = s.pocket as { p10: number; p50: number; p90: number } | undefined;
    out.push({
      title: "Protection and rush",
      rows: [
        ["Pocket held, 10 / 50 / 90%", pocket ? `${f(pocket.p10, 2)}  ${f(pocket.p50, 2)}  ${f(pocket.p90, 2)} s` : "–"],
        ["Pressure", pct(s.pressured, s.plays)],
        ["Sacks", pct(s.sacks, s.plays, 1)],
        ["Pass rush: sheds / moves won / pancakes per play", `${per(Number(s.sheds), s.plays)} / ${per(Number(s.rush_moves), s.plays)} / ${per(Number(s.pancakes), s.plays)}`],
        ["Holds / hot reads", `${s.holds} / ${s.hot}`],
        ["Scrambles", pct(s.scrambles, s.plays)],
      ],
    });
  }

  out.push({
    title: "Tackling",
    rows: [
      ["Tries / broken / missed / moves won, per play", `${per(Number(s.tackle_tries), s.plays)} / ${per(Number(s.broken), s.plays)} / ${per(Number(s.missed), s.plays)} / ${per(Number(s.evaded), s.plays)}`],
      ["Yards before / after contact", `${f(Number(s.contacted) > 0 ? Number(s.before_contact) / Number(s.contacted) : null)} / ${f(Number(s.contacted) > 0 ? Number(s.after_contact) / Number(s.contacted) : null)}`],
      ["Fumbles (lost)", `${s.fumbles} (${s.fumbles_lost})`],
    ],
  });

  const targets = Object.entries((s.by_target ?? {}) as Record<string, { thrown: number; complete: number; picks: number; yards: number }>);
  if (targets.length) {
    out.push({
      title: "Thrown to",
      rows: targets.map(([slot, t]) => [slot, `${t.thrown} thrown, ${pct(t.complete, t.thrown)} complete, ${t.picks} int, ${f(t.thrown > 0 ? t.yards / t.thrown : null)} yd`]),
    });
  }

  const openness = (s.openness ?? []) as [number, number][];
  const rows: [string, string][] = [];
  openness.forEach(([thrown, done], b) => {
    if (thrown > 0) rows.push([`${(b * 0.2).toFixed(1)}–${((b + 1) * 0.2).toFixed(1)}`, `${done}/${thrown} completed (${pct(done, thrown)})`]);
  });
  if (rows.length) out.push({ title: "Openness at the throw", rows });

  const reads = Object.entries((s.reads ?? {}) as Record<string, number>);
  if (reads.length) out.push({ title: "Run reads", rows: reads.map(([name, n]) => [name, String(n)]) });

  return out;
}
