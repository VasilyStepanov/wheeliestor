#!/usr/bin/env python3
"""Turns the JSON that bench/run.sh keeps into a table and a chart.

    bench/report.py results/matrix --plot results/matrix/libc-vs-others.svg

The variant of a result (libc, alibc, uring, uring-ms) and the load it was
measured under are read from the options fio recorded in the JSON, not from the
name of the file, so results from any runs can be put together. A result that
was measured more than once is taken from the latest run.

The table has a row for each load and a column for each variant, and a cell is
`IOPS / MiB/s / stddev%`, the spread of IOPS over the run: the number is no
good where the spread is large (above about 20%).

The chart has a point for each load, with the baseline (libc) on the
horizontal axis and another variant on the vertical one, both logarithmic:
above the diagonal the variant is faster than the baseline, below it slower.
Loads with small blocks are measured in IOPS and loads with large blocks in
MiB/s, so they have a panel each.
"""

import argparse
import glob
import json
import math
import os
import sys
from xml.sax.saxutils import escape

BASELINE = "libc"
VARIANT_ORDER = ["libc", "alibc", "uring", "uring-ms"]
RW_ORDER = ["randread", "randwrite", "randrw", "read", "write", "rw"]

# The series of the chart: a variant keeps its colour and its shape whatever
# else is on the chart. Three is as many as a scatter chart can tell apart.
SERIES = [("uring", "circle"), ("uring-ms", "square"), ("alibc", "triangle")]

# Blocks of this size or more are measured in MiB/s, smaller ones in IOPS.
LARGE_BLOCK = 128 * 1024


def size_bytes(text):
    text = text.strip().lower()
    units = {"k": 1024, "m": 1024**2, "g": 1024**3}
    if text and text[-1] in units:
        return int(float(text[:-1]) * units[text[-1]])
    return int(text)


class Result:
    def __init__(self, job, timestamp):
        options = job["job options"]
        backend = options.get("wheelie_backend")
        if backend is None:
            raise ValueError("not a result of the wheelie engine")
        multishot = options.get("wheelie_multishot", "0") not in ("0", "")
        self.variant = backend + ("-ms" if multishot else "")
        self.rw = options["rw"]
        self.bs = options["bs"]
        self.depth = int(options["iodepth"])
        self.numjobs = int(options["numjobs"])
        self.sync = int(options.get("sync", "0") or 0)
        self.timestamp = timestamp

        self.iops = 0.0
        self.mibs = 0.0
        self.spread = 0.0
        for direction in ("read", "write"):
            stats = job[direction]
            if stats["io_bytes"] == 0:
                continue
            self.iops += stats["iops"]
            self.mibs += stats["bw"] / 1024
            if stats["iops_mean"]:
                self.spread = max(
                    self.spread, 100 * stats["iops_stddev"] / stats["iops_mean"]
                )

    @property
    def load(self):
        return (self.rw, self.bs, self.depth, self.numjobs, self.sync)

    def cell(self):
        return "%d / %.1f / %.1f%%" % (round(self.iops), self.mibs, self.spread)


def load_label(load):
    rw, bs, depth, numjobs, sync = load
    return "%s %s qd%d nj%d%s" % (rw, bs, depth, numjobs, " sync" if sync else "")


def load_sort_key(load):
    rw, bs, depth, numjobs, sync = load
    rank = RW_ORDER.index(rw) if rw in RW_ORDER else len(RW_ORDER)
    return (rank, size_bytes(bs), depth, numjobs, sync)


def read_results(paths):
    """{load: {variant: Result}} from the JSON files, or directories of them."""
    files = []
    for path in paths:
        if os.path.isdir(path):
            files += sorted(glob.glob(os.path.join(path, "*.json")))
        else:
            files.append(path)

    results = {}
    for path in files:
        try:
            with open(path) as handle:
                data = json.load(handle)
            result = Result(data["jobs"][0], data.get("timestamp_ms", 0))
        except (ValueError, KeyError, IndexError) as error:
            print("report.py: skipping %s: %s" % (path, error), file=sys.stderr)
            continue
        by_variant = results.setdefault(result.load, {})
        known = by_variant.get(result.variant)
        if known is None or result.timestamp >= known.timestamp:
            by_variant[result.variant] = result
    return results


def variants_of(results):
    present = {variant for by_variant in results.values() for variant in by_variant}
    ordered = [variant for variant in VARIANT_ORDER if variant in present]
    return ordered + sorted(present - set(ordered))


def markdown_table(header, rows):
    widths = [
        max(len(row[i]) for row in [header] + rows) for i in range(len(header))
    ]

    def line(row):
        cells = [row[0].ljust(widths[0])]
        cells += [cell.rjust(width) for cell, width in zip(row[1:], widths[1:])]
        return "| " + " | ".join(cells) + " |"

    rule = ["-" * widths[0]] + ["-" * (width - 1) + ":" for width in widths[1:]]
    return "\n".join(
        [line(header), "| " + " | ".join(rule) + " |"] + [line(row) for row in rows]
    )


def results_table(results, variants):
    rows = []
    for load in sorted(results, key=load_sort_key):
        row = [load_label(load)]
        for variant in variants:
            result = results[load].get(variant)
            row.append(result.cell() if result else "-")
        rows.append(row)
    return markdown_table(["load"] + variants, rows)


def ratio_table(results, variants, baseline):
    others = [variant for variant in variants if variant != baseline]
    rows = []
    for load in sorted(results, key=load_sort_key):
        base = results[load].get(baseline)
        row = [load_label(load)]
        for variant in others:
            result = results[load].get(variant)
            if base and result and base.iops:
                row.append("x%.2f" % (result.iops / base.iops))
            else:
                row.append("-")
        rows.append(row)
    return markdown_table(["load, IOPS against " + baseline] + others, rows)


# -- the chart -----------------------------------------------------------------

STYLE = """
  :root {
    --surface: #fcfcfb; --ink: #0b0b0b; --ink-2: #52514e; --ink-3: #75736d;
    --grid: #ebeae6; --axis: #d6d5cf;
    --s1: #2a78d6; --s2: #eb6834; --s3: #1baf7a;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --surface: #1a1a19; --ink: #f0efec; --ink-2: #c3c2b7; --ink-3: #a09f97;
      --grid: #2c2c2a; --axis: #3f3f3c;
      --s1: #3987e5; --s2: #d95926; --s3: #199e70;
    }
  }
  text { font-family: system-ui, -apple-system, "Segoe UI", sans-serif; fill: var(--ink-2); font-size: 12px; }
  .title { fill: var(--ink); font-size: 16px; font-weight: 600; }
  .subtitle { fill: var(--ink-3); }
  .panel-title { fill: var(--ink); font-weight: 600; }
  .tick { fill: var(--ink-3); font-size: 11px; }
  .axis-label { fill: var(--ink-2); }
  .point-label { fill: var(--ink-2); font-size: 11px; paint-order: stroke;
                 stroke: var(--surface); stroke-width: 3px; stroke-linejoin: round; }
  .surface { fill: var(--surface); }
  .grid { stroke: var(--grid); stroke-width: 1; fill: none; }
  .axis { stroke: var(--axis); stroke-width: 1; fill: none; }
  .diagonal { stroke: var(--ink-3); stroke-width: 1; fill: none; }
  .mark { stroke: var(--surface); stroke-width: 2; }
  .s1 { fill: var(--s1); } .s2 { fill: var(--s2); } .s3 { fill: var(--s3); }
  .hit { fill: transparent; stroke: none; }
  .pt:hover .mark, .pt:focus .mark { stroke: var(--ink); }
  .pt:focus { outline: none; }
"""


def compact(value):
    if value >= 1e6:
        return "%gM" % (value / 1e6)
    if value >= 1e3:
        return "%gK" % (value / 1e3)
    return "%g" % value


def nice_bounds(smallest, largest):
    """The nearest 1, 2 or 5 of a decade below, and above, the values, with a
    little room around them: a chart zoomed to its data, not to its decades."""
    steps = [10**power * step for power in range(-3, 12) for step in (1, 2, 5)]
    low = max(step for step in steps if step <= smallest * 0.8)
    high = min(step for step in steps if step >= largest * 1.25)
    return low, high


def log_ticks(low, high):
    """Ticks at 1, 2 and 5 of every decade in [low, high]."""
    ticks = []
    decade = 10 ** math.floor(math.log10(low))
    while decade <= high * 1.0001:
        for step in (1, 2, 5):
            value = decade * step
            if low * 0.9999 <= value <= high * 1.0001:
                ticks.append(value)
        decade *= 10
    return ticks


def marker(shape, x, y, css):
    if shape == "square":
        return '<rect class="mark %s" x="%.1f" y="%.1f" width="10" height="10"/>' % (
            css, x - 5, y - 5)
    if shape == "triangle":
        return '<polygon class="mark %s" points="%.1f,%.1f %.1f,%.1f %.1f,%.1f"/>' % (
            css, x, y - 6, x + 6, y + 5, x - 6, y + 5)
    return '<circle class="mark %s" cx="%.1f" cy="%.1f" r="5"/>' % (css, x, y)


def panel_points(results, baseline, series, large):
    """The points of a panel: [(series index, load, x, y)]."""
    points = []
    for load, by_variant in results.items():
        if (size_bytes(load[1]) >= LARGE_BLOCK) != large:
            continue
        base = by_variant.get(baseline)
        if base is None:
            continue
        x = base.mibs if large else base.iops
        for index, (variant, _) in enumerate(series):
            result = by_variant.get(variant)
            if result is None:
                continue
            y = result.mibs if large else result.iops
            if x > 0 and y > 0:
                points.append((index, load, x, y))
    return points


def draw_panel(out, points, series, baseline, left, top, size, title, unit):
    values = [value for point in points for value in point[2:4]]
    low, high = nice_bounds(min(values), max(values))
    span = math.log10(high) - math.log10(low)

    def px(value):
        return left + (math.log10(value) - math.log10(low)) / span * size

    def py(value):
        return top + size - (math.log10(value) - math.log10(low)) / span * size

    out.append('<text class="panel-title" x="%d" y="%d">%s</text>' % (left, top - 14, escape(title)))
    ticks = log_ticks(low, high)
    if span > 2.5:
        ticks = [t for t in ticks if round(math.log10(t) % 1, 6) in (0, 1)]
    for tick in ticks:
        out.append('<line class="grid" x1="%.1f" y1="%d" x2="%.1f" y2="%d"/>' % (px(tick), top, px(tick), top + size))
        out.append('<line class="grid" x1="%d" y1="%.1f" x2="%d" y2="%.1f"/>' % (left, py(tick), left + size, py(tick)))
        out.append('<text class="tick" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (px(tick), top + size + 16, compact(tick)))
        out.append('<text class="tick" x="%d" y="%.1f" text-anchor="end">%s</text>' % (left - 8, py(tick) + 4, compact(tick)))
    out.append('<rect class="axis" x="%d" y="%d" width="%d" height="%d"/>' % (left, top, size, size))
    out.append('<line class="diagonal" x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f"/>' % (px(low), py(low), px(high), py(high)))
    out.append('<text class="axis-label" x="%.1f" y="%d" text-anchor="middle">%s, %s</text>' % (left + size / 2, top + size + 38, escape(baseline), unit))
    out.append('<text class="axis-label" transform="translate(%d %.1f) rotate(-90)" text-anchor="middle">the variant, %s</text>' % (left - 46, top + size / 2, unit))

    # The loads furthest from the diagonal, each way, are named on the chart;
    # every point has its numbers in its tooltip, and the table has them all.
    ratios = sorted(points, key=lambda point: point[3] / point[2])
    named = {id(ratios[0]), id(ratios[-1])} if len(ratios) > 1 else set()

    labels = []
    for point in points:
        index, load, x, y = point
        variant, shape = series[index]
        css = "s%d" % (index + 1)
        cx, cy = px(x), py(y)
        out.append('<g class="pt" tabindex="0">')
        out.append("<title>%s</title>" % escape("%s, %s: %s %s against %s %s (x%.2f)" % (
            variant, load_label(load), compact(round(y)), unit, compact(round(x)), baseline, y / x)))
        out.append('<circle class="hit" cx="%.1f" cy="%.1f" r="12"/>' % (cx, cy))
        out.append(marker(shape, cx, cy, css))
        if id(point) in named:
            right = cx > left + size * 0.55
            labels.append('<text class="point-label" x="%.1f" y="%.1f" text-anchor="%s">%s</text>' % (
                cx + (-12 if right else 12), cy + 4, "end" if right else "start",
                escape("%s, x%.1f" % (load_label(load), y / x))))
        out.append("</g>")
    # The labels are on top of every mark, not under the ones drawn after them.
    out.extend(labels)


def chart(results, baseline, variants):
    series = [(v, shape) for v, shape in SERIES if v in variants]
    panels = []
    for large, title, unit in ((False, "Small blocks", "IOPS"), (True, "Large blocks", "MiB/s")):
        points = panel_points(results, baseline, series, large)
        if points:
            panels.append((points, title, unit))
    if not panels:
        raise ValueError("no load has a result for %s and another variant" % baseline)

    size, gap, left, top = 340, 110, 80, 130
    width = left + len(panels) * size + (len(panels) - 1) * gap + 40
    height = top + size + 70
    names = ", ".join(v for v, _ in series)
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" role="img" aria-labelledby="t d">' % (width, height, width, height),
        "<style>%s</style>" % STYLE,
        '<title id="t">%s against the other variants</title>' % escape(baseline),
        '<desc id="d">A point for each load: %s on the horizontal axis and %s on the vertical one, both logarithmic. Above the diagonal a variant is faster than %s.</desc>' % (escape(baseline), escape(names), escape(baseline)),
        '<rect class="surface" width="%d" height="%d"/>' % (width, height),
        '<text class="title" x="24" y="32">%s against the other variants</text>' % escape(baseline),
        '<text class="subtitle" x="24" y="52">A point is one load. Above the diagonal a variant is faster than %s, below it slower.</text>' % escape(baseline),
    ]

    # The legend: a mark and a name for each series, and the diagonal.
    x = 24
    for index, (variant, shape) in enumerate(series):
        out.append(marker(shape, x + 6, 78, "s%d" % (index + 1)))
        out.append('<text x="%d" y="82">%s</text>' % (x + 18, escape(variant)))
        x += 30 + 8 * len(variant)
    out.append('<line class="diagonal" x1="%d" y1="78" x2="%d" y2="78"/>' % (x, x + 22))
    out.append('<text x="%d" y="82">same as %s</text>' % (x + 30, escape(baseline)))

    for number, (points, title, unit) in enumerate(panels):
        draw_panel(out, points, series, baseline, left + number * (size + gap), top, size, title, unit)
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(
        description="A table and a chart from the results of bench/run.sh.")
    parser.add_argument("paths", nargs="+", help="JSON files, or directories of them")
    parser.add_argument("--baseline", default=BASELINE, help="the variant the others are compared with (default: %s)" % BASELINE)
    parser.add_argument("--plot", metavar="FILE", help="write the chart, an SVG, to FILE")
    parser.add_argument("--ratio", action="store_true", help="also print the IOPS of each variant as a multiple of the baseline")
    args = parser.parse_args()

    results = read_results(args.paths)
    if not results:
        print("report.py: no results", file=sys.stderr)
        return 1
    variants = variants_of(results)

    print(results_table(results, variants))
    if args.ratio:
        print()
        print(ratio_table(results, variants, args.baseline))

    if args.plot:
        try:
            svg = chart(results, args.baseline, variants)
        except ValueError as error:
            print("report.py: %s" % error, file=sys.stderr)
            return 1
        with open(args.plot, "w") as handle:
            handle.write(svg)
        print("\nchart: %s" % args.plot, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
