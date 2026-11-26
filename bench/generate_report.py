#!/usr/bin/env python3
"""Generate Markdown report and SVG charts from benchmark summary.json."""

from __future__ import annotations

import argparse
import html
import json
from pathlib import Path

METHOD_LABELS = {
    "fct_t1": "fct (-t 1)",
    "fct_t4": "fct (-t 4)",
    "fct_t1_z": "fct (-t 1 -z)",
    "fct_t4_z": "fct (-t 4 -z)",
    "rsync_raw": "rsync (raw)",
    "rsync_z_default": "rsync -z (zstd)",
    "rsync_z_zlib": "rsync -z (zlib-1)",
    "e2e_gzip1": "E2E gzip-1",
    "e2e_zstd": "E2E zstd",
}

METHOD_COLORS = {
    "fct_t1": "#2563eb",
    "fct_t4": "#0ea5e9",
    "fct_t1_z": "#7c3aed",
    "fct_t4_z": "#a855f7",
    "rsync_raw": "#64748b",
    "rsync_z_default": "#f59e0b",
    "rsync_z_zlib": "#d97706",
    "e2e_gzip1": "#ef4444",
    "e2e_zstd": "#10b981",
}

PROFILE_TITLES = {
    "wan_100m": "WAN 100 Mbps / 20 ms RTT",
    "lan_1g": "LAN 1 Gbps / 2 ms RTT",
    "unlimited": "Unlimited veth Baseline",
}

ORDERED_FILES = ["compressible.log", "mixed.tar", "archive.gz", "random.bin"]
ORDERED_METHODS = [
    "fct_t1",
    "fct_t4",
    "fct_t1_z",
    "fct_t4_z",
    "rsync_raw",
    "rsync_z_default",
    "rsync_z_zlib",
    "e2e_gzip1",
    "e2e_zstd",
]


def nice_max(val: float) -> float:
    if val <= 0:
        return 1.0
    exp = 10 ** (len(str(int(val))) - 1) if val >= 1 else 0.1
    for m in (1, 1.2, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        cand = m * exp
        if cand >= val * 1.12:
            return cand
    return val * 1.2


def svg_header(width: int, height: int) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" style="font-family: -apple-system, BlinkMacSystemFont, Segoe UI, Roboto, sans-serif; background: #ffffff;">'
    ]


def render_walltime_svg(profile: str, profile_data: dict, out_path: Path) -> None:
    files = [f for f in ORDERED_FILES if f in profile_data]
    methods = [m for m in ["fct_t1", "fct_t4", "rsync_raw", "rsync_z_default", "rsync_z_zlib", "e2e_gzip1", "e2e_zstd"]
               if any(m in profile_data[f] for f in files)]

    width, height = 980, 520
    margin_l, margin_r, margin_t, margin_b = 75, 30, 80, 95
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    max_val = max(
        (profile_data[f][m]["median_total_s"] for f in files for m in methods if m in profile_data[f]),
        default=1.0,
    )
    y_max = nice_max(max_val)

    lines = svg_header(width, height)
    title = f"Median End-to-End Transfer Time — {PROFILE_TITLES.get(profile, profile)}"
    lines.append(f'<text x="{width // 2}" y="30" text-anchor="middle" font-size="18" font-weight="700" fill="#0f172a">{html.escape(title)}</text>')
    lines.append(f'<text x="{width // 2}" y="50" text-anchor="middle" font-size="12" fill="#475569">Lower is better (seconds, median of measured runs)</text>')

    # Horizontal gridlines
    ticks = 5
    for i in range(ticks + 1):
        val = y_max * i / ticks
        y = margin_t + plot_h - (plot_h * i / ticks)
        lines.append(f'<line x1="{margin_l}" y1="{y:.1f}" x2="{margin_l + plot_w}" y2="{y:.1f}" stroke="#e2e8f0" stroke-width="1"/>')
        lines.append(f'<text x="{margin_l - 10}" y="{y + 4:.1f}" text-anchor="end" font-size="11" fill="#475569">{val:.1f}s</text>')

    lines.append(f'<line x1="{margin_l}" y1="{margin_t}" x2="{margin_l}" y2="{margin_t + plot_h}" stroke="#94a3b8" stroke-width="1.2"/>')
    lines.append(f'<line x1="{margin_l}" y1="{margin_t + plot_h}" x2="{margin_l + plot_w}" y2="{margin_t + plot_h}" stroke="#94a3b8" stroke-width="1.2"/>')

    group_w = plot_w / max(1, len(files))
    inner_w = group_w * 0.80
    bar_w = inner_w / max(1, len(methods))

    for gi, fname in enumerate(files):
        gx = margin_l + gi * group_w + (group_w - inner_w) / 2
        cx = margin_l + (gi + 0.5) * group_w
        lines.append(f'<text x="{cx:.1f}" y="{margin_t + plot_h + 22}" text-anchor="middle" font-size="12" font-weight="600" fill="#1e293b">{html.escape(fname)}</text>')
        for mi, method in enumerate(methods):
            if method not in profile_data[fname]:
                continue
            val = profile_data[fname][method]["median_total_s"]
            bh = (val / y_max) * plot_h if y_max > 0 else 0
            bx = gx + mi * bar_w + 1
            bw = max(4.0, bar_w - 2)
            by = margin_t + plot_h - bh
            color = METHOD_COLORS.get(method, "#64748b")
            lines.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" height="{bh:.1f}" fill="{color}" rx="2"/>')
            if bh > 12:
                lines.append(f'<text x="{bx + bw / 2:.1f}" y="{by - 4:.1f}" text-anchor="middle" font-size="9.5" fill="#334155">{val:.1f}</text>')

    # Legend at bottom
    lx = margin_l
    ly = height - 38
    for method in methods:
        label = METHOD_LABELS.get(method, method)
        color = METHOD_COLORS.get(method, "#64748b")
        lines.append(f'<rect x="{lx}" y="{ly}" width="12" height="12" fill="{color}" rx="2"/>')
        lines.append(f'<text x="{lx + 16}" y="{ly + 10}" font-size="11" fill="#1e293b">{html.escape(label)}</text>')
        lx += max(95, len(label) * 7 + 28)

    lines.append("</svg>\n")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def render_e2e_breakdown_svg(profile: str, profile_data: dict, out_path: Path) -> None:
    files = [f for f in ORDERED_FILES if f in profile_data]
    compare_methods = ["fct_t1", "fct_t4", "e2e_gzip1", "e2e_zstd"]

    width, height = 980, 500
    margin_l, margin_r, margin_t, margin_b = 75, 30, 80, 95
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    max_val = max(
        (profile_data[f][m]["median_total_s"] for f in files for m in compare_methods if m in profile_data[f]),
        default=1.0,
    )
    y_max = nice_max(max_val)

    lines = svg_header(width, height)
    title = f"3-Step E2E Stage Breakdown vs fct — {PROFILE_TITLES.get(profile, profile)}"
    lines.append(f'<text x="{width // 2}" y="30" text-anchor="middle" font-size="18" font-weight="700" fill="#0f172a">{html.escape(title)}</text>')
    lines.append(f'<text x="{width // 2}" y="50" text-anchor="middle" font-size="12" fill="#475569">Stacked: Compress + Transfer + Decompress vs integrated fct pipeline</text>')

    ticks = 5
    for i in range(ticks + 1):
        val = y_max * i / ticks
        y = margin_t + plot_h - (plot_h * i / ticks)
        lines.append(f'<line x1="{margin_l}" y1="{y:.1f}" x2="{margin_l + plot_w}" y2="{y:.1f}" stroke="#e2e8f0" stroke-width="1"/>')
        lines.append(f'<text x="{margin_l - 10}" y="{y + 4:.1f}" text-anchor="end" font-size="11" fill="#475569">{val:.1f}s</text>')

    lines.append(f'<line x1="{margin_l}" y1="{margin_t}" x2="{margin_l}" y2="{margin_t + plot_h}" stroke="#94a3b8" stroke-width="1.2"/>')
    lines.append(f'<line x1="{margin_l}" y1="{margin_t + plot_h}" x2="{margin_l + plot_w}" y2="{margin_t + plot_h}" stroke="#94a3b8" stroke-width="1.2"/>')

    group_w = plot_w / max(1, len(files))
    inner_w = group_w * 0.72
    bar_w = inner_w / len(compare_methods)

    for gi, fname in enumerate(files):
        gx = margin_l + gi * group_w + (group_w - inner_w) / 2
        cx = margin_l + (gi + 0.5) * group_w
        lines.append(f'<text x="{cx:.1f}" y="{margin_t + plot_h + 24}" text-anchor="middle" font-size="12" font-weight="600" fill="#1e293b">{html.escape(fname)}</text>')

        for mi, m in enumerate(compare_methods):
            if m not in profile_data[fname]:
                continue
            d = profile_data[fname][m]
            bx = gx + mi * bar_w + 4
            bw = max(10.0, bar_w - 8)
            total_s = d["median_total_s"]

            if m.startswith("e2e_"):
                cs = d.get("median_compress_s", 0.0)
                xs = d.get("median_transfer_s", 0.0)
                ds = d.get("median_decompress_s", 0.0)
                y_cur = margin_t + plot_h
                for seg_val, seg_color in ((cs, "#f59e0b"), (xs, "#64748b"), (ds, "#ef4444")):
                    sh = (seg_val / y_max) * plot_h
                    y_cur -= sh
                    lines.append(f'<rect x="{bx:.1f}" y="{y_cur:.1f}" width="{bw:.1f}" height="{sh:.1f}" fill="{seg_color}"/>')
                lines.append(f'<text x="{bx + bw / 2:.1f}" y="{y_cur - 4:.1f}" text-anchor="middle" font-size="10" fill="#1e293b">{total_s:.1f}s</text>')
            else:
                bh = (total_s / y_max) * plot_h
                by = margin_t + plot_h - bh
                color = METHOD_COLORS[m]
                lines.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" height="{bh:.1f}" fill="{color}" rx="2"/>')
                lines.append(f'<text x="{bx + bw / 2:.1f}" y="{by - 4:.1f}" text-anchor="middle" font-size="10" fill="#1e293b">{total_s:.1f}s</text>')

            short = {"fct_t1": "t1", "fct_t4": "t4", "e2e_gzip1": "gz1", "e2e_zstd": "zst"}[m]
            lines.append(f'<text x="{bx + bw / 2:.1f}" y="{margin_t + plot_h + 12}" text-anchor="middle" font-size="9.5" fill="#475569">{short}</text>')

    legend_items = [
        ("fct (-t 1)", "#2563eb"),
        ("fct (-t 4)", "#0ea5e9"),
        ("E2E: Compress", "#f59e0b"),
        ("E2E: Transfer", "#64748b"),
        ("E2E: Decompress+fsync", "#ef4444"),
    ]
    lx = margin_l
    ly = height - 35
    for label, color in legend_items:
        lines.append(f'<rect x="{lx}" y="{ly}" width="12" height="12" fill="{color}" rx="2"/>')
        lines.append(f'<text x="{lx + 16}" y="{ly + 10}" font-size="11" fill="#1e293b">{html.escape(label)}</text>')
        lx += len(label) * 7 + 34

    lines.append("</svg>\n")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def render_autobypass_svg(results: dict, out_path: Path) -> None:
    profiles = [p for p in ("wan_100m", "lan_1g", "unlimited") if p in results]
    target_files = ["archive.gz", "random.bin"]
    methods = ["fct_t1", "fct_t1_z", "fct_t4", "fct_t4_z", "rsync_z_default", "rsync_z_zlib"]

    width, height = 980, 480
    margin_l, margin_r, margin_t, margin_b = 75, 30, 80, 95
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    combos = [(p, f) for p in profiles for f in target_files if f in results.get(p, {})]
    if not combos:
        return

    max_val = max(
        (results[p][f][m]["median_total_s"] for p, f in combos for m in methods if m in results[p][f]),
        default=1.0,
    )
    y_max = nice_max(max_val)

    lines = svg_header(width, height)
    lines.append(f'<text x="{width // 2}" y="30" text-anchor="middle" font-size="18" font-weight="700" fill="#0f172a">Adaptive Compression Auto-Bypass vs Forced -z &amp; rsync -z</text>')
    lines.append(f'<text x="{width // 2}" y="50" text-anchor="middle" font-size="12" fill="#475569">Incompressible payloads (archive.gz, random.bin) — lower is better</text>')

    ticks = 5
    for i in range(ticks + 1):
        val = y_max * i / ticks
        y = margin_t + plot_h - (plot_h * i / ticks)
        lines.append(f'<line x1="{margin_l}" y1="{y:.1f}" x2="{margin_l + plot_w}" y2="{y:.1f}" stroke="#e2e8f0" stroke-width="1"/>')
        lines.append(f'<text x="{margin_l - 10}" y="{y + 4:.1f}" text-anchor="end" font-size="11" fill="#475569">{val:.1f}s</text>')

    lines.append(f'<line x1="{margin_l}" y1="{margin_t}" x2="{margin_l}" y2="{margin_t + plot_h}" stroke="#94a3b8" stroke-width="1.2"/>')
    lines.append(f'<line x1="{margin_l}" y1="{margin_t + plot_h}" x2="{margin_l + plot_w}" y2="{margin_t + plot_h}" stroke="#94a3b8" stroke-width="1.2"/>')

    group_w = plot_w / len(combos)
    inner_w = group_w * 0.78
    bar_w = inner_w / len(methods)

    for gi, (prof, fname) in enumerate(combos):
        gx = margin_l + gi * group_w + (group_w - inner_w) / 2
        cx = margin_l + (gi + 0.5) * group_w
        lines.append(f'<text x="{cx:.1f}" y="{margin_t + plot_h + 18}" text-anchor="middle" font-size="11" font-weight="600" fill="#1e293b">{html.escape(prof)}</text>')
        lines.append(f'<text x="{cx:.1f}" y="{margin_t + plot_h + 32}" text-anchor="middle" font-size="10.5" fill="#475569">{html.escape(fname)}</text>')

        for mi, m in enumerate(methods):
            if m not in results[prof][fname]:
                continue
            val = results[prof][fname][m]["median_total_s"]
            bh = (val / y_max) * plot_h
            bx = gx + mi * bar_w + 1
            bw = max(4.0, bar_w - 2)
            by = margin_t + plot_h - bh
            color = METHOD_COLORS[m]
            lines.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" height="{bh:.1f}" fill="{color}" rx="2"/>')
            lines.append(f'<text x="{bx + bw / 2:.1f}" y="{by - 4:.1f}" text-anchor="middle" font-size="9" fill="#334155">{val:.1f}</text>')

    lx = margin_l
    ly = height - 35
    for m in methods:
        label = METHOD_LABELS[m]
        color = METHOD_COLORS[m]
        lines.append(f'<rect x="{lx}" y="{ly}" width="12" height="12" fill="{color}" rx="2"/>')
        lines.append(f'<text x="{lx + 16}" y="{ly + 10}" font-size="11" fill="#1e293b">{html.escape(label)}</text>')
        lx += len(label) * 7 + 34

    lines.append("</svg>\n")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def generate_report(output_dir: Path) -> None:
    summary_path = output_dir / "summary.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    charts_dir = output_dir / "charts"
    charts_dir.mkdir(parents=True, exist_ok=True)

    results = summary["results"]
    manifest = summary["manifest"]
    sys_meta = summary["system"]
    cfg = summary["config"]

    chart_links: list[tuple[str, str]] = []
    for profile in cfg["profiles"]:
        if profile not in results:
            continue
        svg_name = f"walltime_{profile}.svg"
        render_walltime_svg(profile, results[profile], charts_dir / svg_name)
        chart_links.append((f"{PROFILE_TITLES.get(profile, profile)} Wall Time", f"charts/{svg_name}"))

    for profile in ("wan_100m", "lan_1g"):
        if profile in results:
            svg_name = f"e2e_breakdown_{profile}.svg"
            render_e2e_breakdown_svg(profile, results[profile], charts_dir / svg_name)
            chart_links.append((f"{PROFILE_TITLES.get(profile, profile)} E2E Breakdown", f"charts/{svg_name}"))

    if any(f in results.get(p, {}) for p in results for f in ("archive.gz", "random.bin")):
        render_autobypass_svg(results, charts_dir / "autobypass_comparison.svg")
        chart_links.append(("Auto-Bypass vs Forced -z Comparison", "charts/autobypass_comparison.svg"))

    md: list[str] = []
    md.append("# fct vs rsync End-to-End Performance Benchmark Report\n")
    md.append(f"- **Generated**: `{summary['generated_at']}`")
    md.append(
        f"- **Config**: `{cfg['size_mb']} MiB` single files, "
        f"`{cfg['warmup_runs']}` warmup + `{cfg['measured_runs']}` measured runs, "
        f"durability parity (`fsync` enabled on `fct`, `rsync --fsync`, and 3-step E2E output)"
    )
    md.append(
        f"- **System**: {sys_meta['cpu_count']} vCPUs, {sys_meta['mem_gib']} GiB RAM, "
        f"Linux `{sys_meta['kernel']}` (`{sys_meta['arch']}`)"
    )
    md.append(f"- **Tools**: `{sys_meta['gcc_version']}` | `{sys_meta['rsync_version']}` | `{sys_meta['zstd_version']}` | `{sys_meta['gzip_version']}`\n")

    md.append("## 1. Dataset Characteristics\n")
    md.append("| File | Size (MiB) | `gzip -1` Ratio | `zstd` Ratio | Chunk 0 (2 MiB) `gzip -1` | Expected `fct` Mode |")
    md.append("| --- | ---: | ---: | ---: | ---: | --- |")
    for fname in ORDERED_FILES:
        if fname not in manifest["files"]:
            continue
        fm = manifest["files"][fname]
        expected_mode = (
            "Raw bypass (ext/magic)"
            if fname == "archive.gz"
            else ("Raw bypass (trial >95%)" if fname == "random.bin" else "Auto zlib-1")
        )
        md.append(
            f"| `{fname}` | {fm['size_bytes'] / (1024 * 1024):.2f} | "
            f"{fm['gzip1_ratio'] * 100:.1f}% | {fm['zstd_ratio'] * 100:.1f}% | "
            f"{fm['first_chunk_gzip1_ratio'] * 100:.1f}% | {expected_mode} |"
        )
    md.append("")

    md.append("## 2. End-to-End Performance by Network Profile\n")
    for profile in cfg["profiles"]:
        if profile not in results:
            continue
        md.append(f"### {PROFILE_TITLES.get(profile, profile)} (`{profile}`)\n")
        md.append("| File | Method | Median (s) | Mean ± Stddev (s) | Throughput (MiB/s) | Wire TX (MiB) | Wire Ratio | vs `rsync_raw` | vs `e2e_gzip1` |")
        md.append("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for fname in ORDERED_FILES:
            if fname not in results[profile]:
                continue
            fdata = results[profile][fname]
            raw_t = fdata.get("rsync_raw", {}).get("median_total_s")
            e2e_gz_t = fdata.get("e2e_gzip1", {}).get("median_total_s")
            for method in ORDERED_METHODS:
                if method not in fdata:
                    continue
                d = fdata[method]
                sp_raw = f"{raw_t / d['median_total_s']:.2f}x" if raw_t and d["median_total_s"] > 0 else "-"
                sp_e2e = f"{e2e_gz_t / d['median_total_s']:.2f}x" if e2e_gz_t and d["median_total_s"] > 0 else "-"
                wire_mib = d["median_wire_tx_bytes"] / (1024 * 1024)
                md.append(
                    f"| `{fname}` | {METHOD_LABELS.get(method, method)} | "
                    f"**{d['median_total_s']:.3f}** | {d['mean_total_s']:.3f} ± {d['stddev_total_s']:.3f} | "
                    f"{d['throughput_mibs']:.2f} | {wire_mib:.2f} | {d['wire_ratio']:.2f}x | "
                    f"{sp_raw} | {sp_e2e} |"
                )
        md.append("")

    md.append("## 3. 3-Step E2E Breakdown (Compress → Transfer → Decompress + fsync) vs `fct`\n")
    md.append("| Profile | File | Method | Compress (s) | Transfer (s) | Decompress+fsync (s) | Total (s) | `fct_t1` (s) | `fct_t4` (s) | `fct_t4` Speedup |")
    md.append("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for profile in cfg["profiles"]:
        if profile not in results:
            continue
        for fname in ORDERED_FILES:
            if fname not in results[profile]:
                continue
            fdata = results[profile][fname]
            t1 = fdata.get("fct_t1", {}).get("median_total_s")
            t4 = fdata.get("fct_t4", {}).get("median_total_s")
            for m in ("e2e_gzip1", "e2e_zstd"):
                if m not in fdata:
                    continue
                d = fdata[m]
                sp = f"{d['median_total_s'] / t4:.2f}x" if t4 and t4 > 0 else "-"
                t1_s = f"{t1:.3f}" if t1 is not None else "-"
                t4_s = f"{t4:.3f}" if t4 is not None else "-"
                md.append(
                    f"| `{profile}` | `{fname}` | {METHOD_LABELS[m]} | "
                    f"{d['median_compress_s']:.3f} | {d['median_transfer_s']:.3f} | "
                    f"{d['median_decompress_s']:.3f} | **{d['median_total_s']:.3f}** | "
                    f"{t1_s} | {t4_s} | {sp} |"
                )
    md.append("")

    md.append("## 4. Adaptive Auto-Bypass vs Forced `-z` & `rsync -z`\n")
    md.append("| Profile | File | `fct_t1` (auto) | `fct_t1 -z` (forced) | `fct_t4` (auto) | `fct_t4 -z` (forced) | `rsync -z` (zstd) | `rsync -z` (zlib-1) |")
    md.append("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for profile in cfg["profiles"]:
        if profile not in results:
            continue
        for fname in ("archive.gz", "random.bin"):
            if fname not in results[profile]:
                continue
            fdata = results[profile][fname]

            def fmt(m: str) -> str:
                if m not in fdata:
                    return "-"
                d = fdata[m]
                return f"{d['median_total_s']:.3f}s ({d['wire_ratio']:.2f}x)"

            md.append(
                f"| `{profile}` | `{fname}` | {fmt('fct_t1')} | {fmt('fct_t1_z')} | "
                f"{fmt('fct_t4')} | {fmt('fct_t4_z')} | {fmt('rsync_z_default')} | {fmt('rsync_z_zlib')} |"
            )
    md.append("")

    md.append("## 5. Charts\n")
    for title, rel_path in chart_links:
        md.append(f"### {title}\n")
        md.append(f"![{title}]({rel_path})\n")

    (output_dir / "report.md").write_text("\n".join(md), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate benchmark report and SVG charts.")
    parser.add_argument("--output-dir", type=Path, default=Path("bench/results"))
    args = parser.parse_args()
    generate_report(args.output_dir.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
