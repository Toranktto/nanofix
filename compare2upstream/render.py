#!/usr/bin/env python3
"""Render a small README overview table from Google Benchmark JSON.

    render.py [--tables LIST] LABEL=PATH[,PATH...] [LABEL=PATH[,PATH...] ...]

The first column (conventionally `upstream`) is the baseline for deltas.
Multiple paths per label are repetition rounds: cells are medians across
them, and a rep-to-rep spread above 2% on any bench prints a confounded-run
warning to stderr (CLAUDE.md: discard and re-run). Five compact tables:
write/read latency and throughput on the iterator path, the new access path
(build_field_index) vs the upstream iterator as both latency and throughput,
and the index amortization break-even. Not a full per-path dump — the
detailed benches live in `benchmarks/` and are read from raw GB output."""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import re
import statistics
import sys
from typing import Optional


SPREADS: dict[str, dict[str, float]] = {}


def _load_one(path: str) -> dict[str, dict[str, float]]:
    with open(path) as f:
        data = json.load(f)
    benches = data.get("benchmarks", [])
    out: dict[str, dict[str, float]] = {}

    def row(b: dict) -> dict[str, float]:
        return {
            "cpu_time": float(b.get("cpu_time", 0.0)),
            "items_per_second": float(b.get("items_per_second", 0.0)),
            "p95_ns": float(b.get("p95_ns", 0.0)),
            "p99_ns": float(b.get("p99_ns", 0.0)),
            "p999_ns": float(b.get("p999_ns", 0.0)),
            "max_ns": float(b.get("max_ns", 0.0)),
        }

    for b in benches:
        if b.get("aggregate_name") != "mean":
            continue
        name = b.get("run_name") or b["name"].rsplit("_", 1)[0]
        out[name] = row(b)

    for b in benches:
        if b.get("aggregate_name") or b.get("run_type") != "iteration":
            continue
        name = b.get("run_name") or b["name"]
        out.setdefault(name, row(b))
    return out


def load(label: str, pathspec: str) -> dict[str, dict[str, float]]:
    """Median across repetition rounds (comma-separated paths)."""
    reps = [_load_one(p) for p in pathspec.split(",")]
    out: dict[str, dict[str, float]] = {}
    spreads: dict[str, float] = {}
    for name in reps[0]:
        rows = [r[name] for r in reps if name in r]
        out[name] = {k: statistics.median([r[k] for r in rows]) for k in rows[0]}
        vals = [r["cpu_time"] for r in rows if r["cpu_time"] > 0]
        if len(vals) >= 2:
            spreads[name] = (max(vals) - min(vals)) / statistics.median(vals)
    SPREADS[label] = spreads
    return out


MISSING = "—"


def fmt(val: float, unit: str) -> str:
    if val <= 0:
        return MISSING
    if unit == "msgs":
        return (f"{val / 1e6:.3g} M msgs/s" if val >= 1e6
                else f"{val / 1e3:.3g} k msgs/s")
    if unit == "fields":
        return (f"{val / 1e6:.3g} M f/s" if val >= 1e6
                else f"{val / 1e3:.3g} k f/s")
    if unit == "ns":
        return f"{val:.0f} ns" if val >= 1e4 else f"{val:.4g} ns"
    return f"{val:.3g}"


def delta(value: float, baseline: float) -> str:
    if baseline <= 0 or value <= 0:
        return ""
    pct = (value - baseline) / baseline * 100.0
    sign = "+" if pct >= 0 else "-"
    return f" ({sign}{abs(pct):.0f} %)"


def bold(cell: str) -> str:
    if cell == MISSING or cell.startswith("**"):
        return cell
    return f"**{cell}**"


def emit(headers: list[str], rows: list[list[str]]) -> None:
    print("| " + " | ".join(headers) + " |")
    print("| " + " | ".join("---" for _ in headers) + " |")
    for r in rows:
        print("| " + " | ".join(r) + " |")
    print()


def pick_winner(values: list[float], higher_better: bool) -> Optional[int]:
    valid = [(i, v) for i, v in enumerate(values) if v > 0]
    if len(valid) < 2:
        return None
    return (max if higher_better else min)(valid, key=lambda kv: kv[1])[0]


def col_by_label(columns: list[tuple[str, dict]], label: str) -> Optional[dict]:
    for lbl, c in columns:
        if lbl == label:
            return c
    return None


LATENCY_ROWS = [
    ("Write p99",      "BM_Write_TailLatency",                            "p99_ns",  "ns"),
    ("Write p999",     "BM_Write_TailLatency",                            "p999_ns", "ns"),
    ("Read seq p99",   "BM_Parse_TailLatency_Sequential_Iter/synthetic",  "p99_ns",  "ns"),
    ("Read seq p999",  "BM_Parse_TailLatency_Sequential_Iter/synthetic",  "p999_ns", "ns"),
    ("Read rand p99",  "BM_Parse_TailLatency_Random_Iter/synthetic",      "p99_ns",  "ns"),
    ("Read rand p999", "BM_Parse_TailLatency_Random_Iter/synthetic",      "p999_ns", "ns"),
]


def render_latency(columns: list[tuple[str, dict]]) -> None:
    print("Write/read latency — lower is better; first column is the baseline. "
          "Iterator path (`find_with_hint` over a fixed 20-tag set per "
          "message); tail percentiles include the latency probe (RDTSCP "
          "~5-10 cycles on x86-64, steady_clock ~20-30 ns elsewhere):\n")
    out = []
    for label, key, metric, unit in LATENCY_ROWS:
        entries = [c.get(key) for _, c in columns]
        base = entries[0]
        vals, cells = [], []
        for i, e in enumerate(entries):
            v = e[metric] if e else 0.0
            cell = fmt(v, unit)
            if i > 0 and base and base.get(metric, 0.0) > 0:
                cell += delta(v, base[metric])
            cells.append(cell)
            vals.append(v)
        w = pick_winner(vals, higher_better=False)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{label}`"] + cells)
    emit(["Benchmark"] + [lbl for lbl, _ in columns], out)


THROUGHPUT_ROWS = [
    ("Write (NewOrder)", "BM_WriteNewOrder",                   "inv_ns"),
    ("Read seq",         "BM_Parse_Sequential_Iter/synthetic", "ips"),
    ("Read random",      "BM_Parse_Random_Iter/synthetic",     "ips"),
]


def _throughput(entry: Optional[dict], source: str) -> float:
    if not entry:
        return 0.0
    if source == "inv_ns":
        t = entry.get("cpu_time", 0.0)
        return 1e9 / t if t > 0 else 0.0
    return entry.get("items_per_second", 0.0)


def render_throughput(columns: list[tuple[str, dict]]) -> None:
    print("Write/read throughput — higher is better; iterator path, "
          "like-for-like, first column is the baseline:\n")
    out = []
    for label, key, source in THROUGHPUT_ROWS:
        entries = [c.get(key) for _, c in columns]
        base = _throughput(entries[0], source)
        vals, cells = [], []
        for i, e in enumerate(entries):
            v = _throughput(e, source)
            cell = fmt(v, "msgs")
            if i > 0 and base > 0:
                cell += delta(v, base)
            cells.append(cell)
            vals.append(v)
        w = pick_winner(vals, higher_better=True)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{label}`"] + cells)
    emit(["Benchmark"] + [lbl for lbl, _ in columns], out)


NEWAPI_LATENCY_ROWS = [
    ("Read seq p99",
     "BM_Parse_TailLatency_Sequential_Iter/synthetic",
     "BM_Parse_TailLatency_Sequential_Indexed/synthetic", "p99_ns"),
    ("Read seq p999",
     "BM_Parse_TailLatency_Sequential_Iter/synthetic",
     "BM_Parse_TailLatency_Sequential_Indexed/synthetic", "p999_ns"),
    ("Read rand p99",
     "BM_Parse_TailLatency_Random_Iter/synthetic",
     "BM_Parse_TailLatency_Random_Indexed/synthetic", "p99_ns"),
    ("Read rand p999",
     "BM_Parse_TailLatency_Random_Iter/synthetic",
     "BM_Parse_TailLatency_Random_Indexed/synthetic", "p999_ns"),
]

NEWAPI_THROUGHPUT_ROWS = [
    ("Read seq",
     "BM_Parse_Sequential_Iter/synthetic",
     "BM_Parse_Sequential_Indexed/synthetic"),
    ("Read random",
     "BM_Parse_Random_Iter/synthetic",
     "BM_Parse_Random_Indexed/synthetic"),
]


NEWAPI_HEADERS = ["Benchmark", "upstream iterator", "fork indexed",
                  "fork-no-simd indexed"]


def _newapi_cols(columns: list[tuple[str, dict]]):
    upstream = col_by_label(columns, "upstream") or columns[0][1]
    fork = col_by_label(columns, "fork")
    nosimd = col_by_label(columns, "fork-no-simd")
    return upstream, fork, nosimd


def render_newapi_latency(columns: list[tuple[str, dict]]) -> None:
    upstream, fork, nosimd = _newapi_cols(columns)
    if fork is None:
        return
    if not any(fork.get(idx) for _, _, idx, _ in NEWAPI_LATENCY_ROWS):
        return
    print("New access path vs the upstream iterator — latency (ns), lower is "
          "better. Same 20-tag workload as the throughput table below; reading "
          "many tags makes the iterator rescan per lookup, while "
          "`build_field_index` is O(length) regardless of lookup count. Shown "
          "SIMD-on (`fork`) and SIMD-off (`fork-no-simd`). Tail percentiles "
          "include the latency probe (RDTSCP ~5-10 cycles on x86-64, "
          "steady_clock ~20-30 ns elsewhere):\n")
    out = []
    for label, up_key, idx_key, metric in NEWAPI_LATENCY_ROWS:
        up = upstream.get(up_key)
        base = up[metric] if up else 0.0
        cells = [fmt(base, "ns") if base > 0 else MISSING]
        vals = [base]
        for col, key in ((fork, idx_key), (nosimd, idx_key)):
            e = col.get(key) if col else None
            v = e[metric] if e else 0.0
            cell = fmt(v, "ns") + (delta(v, base) if base > 0 and v > 0 else "")
            cells.append(cell)
            vals.append(v)
        w = pick_winner(vals, higher_better=False)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{label}`"] + cells)
    emit(NEWAPI_HEADERS, out)


def render_newapi_throughput(columns: list[tuple[str, dict]]) -> None:
    upstream, fork, nosimd = _newapi_cols(columns)
    if fork is None:
        return
    if not any(fork.get(idx) for _, _, idx in NEWAPI_THROUGHPUT_ROWS):
        return
    print("New access path vs the upstream iterator — message throughput, "
          "higher is better, delta vs upstream's iterator on the same "
          "workload. Shown SIMD-on (`fork`) and SIMD-off (`fork-no-simd`):\n")
    out = []
    for label, up_key, idx_key in NEWAPI_THROUGHPUT_ROWS:
        up = upstream.get(up_key)
        base = up["items_per_second"] if up else 0.0
        cells = [fmt(base, "msgs") if base > 0 else MISSING]
        vals = [base]
        for col, key in ((fork, idx_key), (nosimd, idx_key)):
            e = col.get(key) if col else None
            v = e["items_per_second"] if e else 0.0
            cell = fmt(v, "msgs") + (delta(v, base) if base > 0 and v > 0 else "")
            cells.append(cell)
            vals.append(v)
        w = pick_winner(vals, higher_better=True)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{label}`"] + cells)
    emit(NEWAPI_HEADERS, out)


FINDN_RE = re.compile(r"^BM_Parse_FindN_(Iter|Indexed)/(\d+)/synthetic$")


def _findn_curve(col: dict) -> dict[int, dict[str, float]]:
    """{n: {"Iter": ns_per_msg, "Indexed": ns_per_msg}}, n ascending."""
    curve: dict[int, dict[str, float]] = {}
    for key, entry in col.items():
        m = FINDN_RE.match(key)
        if not m:
            continue
        ips = entry.get("items_per_second", 0.0)
        if ips > 0:
            curve.setdefault(int(m.group(2)), {})[m.group(1)] = 1e9 / ips
    return dict(sorted(curve.items()))


def _break_even(curve: dict[int, dict[str, float]]) -> Optional[str]:
    pts = [(n, v["Iter"] - v["Indexed"]) for n, v in curve.items()
           if "Iter" in v and "Indexed" in v]
    if len(pts) < 2:
        return None
    if pts[0][1] >= 0:
        return f"<= {pts[0][0]}"
    for (n0, d0), (n1, d1) in zip(pts, pts[1:]):
        if d1 >= 0:
            x = n0 + (n1 - n0) * (-d0) / (d1 - d0)
            return f"~{x:.1f}"
    return f"> {pts[-1][0]}"


def render_amortization(columns: list[tuple[str, dict]]) -> None:
    rows, lo, hi = [], None, None
    for label, col in columns:
        curve = {n: v for n, v in _findn_curve(col).items()
                 if "Iter" in v and "Indexed" in v}
        be = _break_even(curve)
        if be is None:
            continue
        ns = sorted(curve)
        lo, hi = ns[0], ns[-1]
        rows.append([f"`{label}`", f"**{be} finds/msg**",
                     fmt(curve[lo]["Iter"], "ns"), fmt(curve[lo]["Indexed"], "ns"),
                     fmt(curve[hi]["Iter"], "ns"), fmt(curve[hi]["Indexed"], "ns")])
    if not rows:
        return
    print("Index amortization — how many `find()`s per message before "
          "`build_field_index` + indexed lookups beat the iterator "
          "(`find_with_hint`), same binary, same messages (per-message ns). "
          "Break-even interpolated from `BM_Parse_FindN_{Iter,Indexed}`; "
          "below it, iterate — above it, index:\n")
    emit(["Config", "Break-even",
          f"iter @{lo}", f"indexed @{lo}", f"iter @{hi}", f"indexed @{hi}"],
         rows)


def parse_column(s: str) -> tuple[str, dict]:
    if "=" not in s:
        raise argparse.ArgumentTypeError(f"column spec must be LABEL=PATH, got {s!r}")
    label, path = s.split("=", 1)
    return label, load(label, path)


RENDERERS = {
    "latency": render_latency,
    "throughput": render_throughput,
    "newapi-latency": render_newapi_latency,
    "newapi-throughput": render_newapi_throughput,
    "amortization": render_amortization,
}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("columns", nargs="+", type=parse_column, metavar="LABEL=PATH",
                    help="Bench JSON columns; first is baseline for deltas.")
    ap.add_argument("--tables", default=",".join(RENDERERS),
                    help=f"Comma list. Options: {','.join(RENDERERS)}. Default: all.")
    ap.add_argument("--out", help="Also write the rendered markdown to this file.")
    ap.add_argument("--header", default="",
                    help="Verbatim markdown prepended to the output.")
    args = ap.parse_args(argv)
    columns: list[tuple[str, dict]] = args.columns

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        if args.header:
            print(args.header.rstrip() + "\n")
        for name in [t.strip() for t in args.tables.split(",") if t.strip()]:
            if name not in RENDERERS:
                print(f"unknown table: {name!r}", file=sys.stderr)
                return 2
            RENDERERS[name](columns)
    sys.stdout.buffer.write(buf.getvalue().encode("utf-8"))
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(buf.getvalue())

    worst = max(((s, name, lbl) for lbl, per in SPREADS.items()
                 for name, s in per.items()), default=None)
    if worst:
        s, name, lbl = worst
        print(f"rep-to-rep spread: max {s * 100:.1f}% ({lbl} {name})",
              file=sys.stderr)
        if s > 0.02:
            print("WARNING: spread exceeds 2% — run may be confounded "
                  "(thermal / layout / background load); discard and re-run.",
                  file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
