#!/usr/bin/env python3
"""Run end-to-end fct vs rsync vs 3-step compress-transfer-decompress benchmarks."""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import selectors
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
ROOT_DIR = BENCH_DIR.parent
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))

import generate_data
import generate_report
from netns_env import PROFILES, BenchNetEnv

DEFAULT_FILES = ["compressible.log", "mixed.tar", "archive.gz", "random.bin"]
DEFAULT_PROFILES = ["wan_100m", "lan_1g", "unlimited"]
BASE_METHODS = [
    "fct_t1",
    "fct_t4",
    "rsync_raw",
    "rsync_z_default",
    "rsync_z_zlib",
    "e2e_gzip1",
    "e2e_zstd",
]
INCOMPRESSIBLE_EXTRA_METHODS = ["fct_t1_z", "fct_t4_z"]
INCOMPRESSIBLE_FILES = {"archive.gz", "random.bin"}

CSV_FIELDS = [
    "profile",
    "file",
    "file_bytes",
    "method",
    "run_index",
    "is_warmup",
    "total_s",
    "compress_s",
    "transfer_s",
    "decompress_s",
    "staged_bytes",
    "wire_tx_bytes",
    "wire_rx_bytes",
    "throughput_mibs",
    "wire_ratio",
    "sha256_ok",
]


def fsync_file(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def clean_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for item in path.iterdir():
        if item.is_dir():
            shutil.rmtree(item)
        else:
            item.unlink()


def wait_for_listening(proc: subprocess.Popen, timeout_s: float = 10.0) -> str:
    """Wait until fct server logs 'Listening on port' on stderr."""
    if proc.stderr is None:
        raise RuntimeError("fct server stderr pipe missing")
    sel = selectors.DefaultSelector()
    sel.register(proc.stderr, selectors.EVENT_READ)
    collected: list[str] = []
    deadline = time.monotonic() + timeout_s
    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                rem = proc.stderr.read()
                collected.append(rem)
                raise RuntimeError(
                    f"fct server exited early (rc={proc.returncode}): {''.join(collected)}"
                )
            remaining = max(0.05, deadline - time.monotonic())
            events = sel.select(timeout=remaining)
            if not events:
                continue
            line = proc.stderr.readline()
            if not line:
                continue
            collected.append(line)
            if "Listening on port" in line:
                return "".join(collected)
    finally:
        sel.close()
    raise TimeoutError(f"Timed out waiting for fct server to listen: {''.join(collected)}")


def run_fct(
    env: BenchNetEnv,
    fct_bin: Path,
    src_path: Path,
    recv_dir: Path,
    port: int,
    workers: int,
    force_compress: bool,
) -> float:
    srv_cmd = [str(fct_bin), "-s", "-p", str(port), "-d", str(recv_dir)]
    srv = env.start_server_popen(
        srv_cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_for_listening(srv, timeout_s=10.0)
        cli_cmd = [
            str(fct_bin),
            "-c",
            env.srv_ip,
            "-p",
            str(port),
            "-f",
            str(src_path),
            "-t",
            str(workers),
        ]
        if force_compress:
            cli_cmd.append("-z")

        t0 = time.perf_counter()
        cli = subprocess.run(cli_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        _, srv_stderr = srv.communicate(timeout=30)
        t1 = time.perf_counter()

        if cli.returncode != 0 or srv.returncode != 0:
            raise RuntimeError(
                f"fct failed (cli_rc={cli.returncode}, srv_rc={srv.returncode})\n"
                f"cli_err={cli.stderr}\nsrv_err={srv_stderr}"
            )
        return t1 - t0
    except Exception:
        if srv.poll() is None:
            srv.kill()
            srv.wait(timeout=5)
        raise


def run_rsync(
    env: BenchNetEnv,
    src_path: Path,
    dest_name: str,
    mode: str,
) -> float:
    dest_url = f"rsync://{env.srv_ip}:{env.rsync_port}/recv/{dest_name}"
    cmd = ["rsync", "--whole-file", "--fsync", "--no-motd", "--quiet"]
    if mode == "rsync_raw":
        cmd.append("--no-compress")
    elif mode == "rsync_z_default":
        cmd.append("-z")
    elif mode == "rsync_z_zlib":
        cmd.extend(["-z", "--compress-choice=zlib", "--compress-level=1"])
    else:
        raise ValueError(f"Unknown rsync mode: {mode}")
    cmd.extend([str(src_path), dest_url])

    t0 = time.perf_counter()
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    t1 = time.perf_counter()
    if proc.returncode != 0:
        raise RuntimeError(f"{mode} failed (rc={proc.returncode}): {proc.stderr}")
    return t1 - t0


def run_e2e_staged(
    env: BenchNetEnv,
    src_path: Path,
    stage_dir: Path,
    recv_dir: Path,
    filename: str,
    codec: str,
) -> tuple[float, float, float, float, int]:
    ext = ".gz" if codec == "gzip1" else ".zst"
    staged_name = f"{filename}{ext}"
    stage_path = stage_dir / staged_name
    recv_staged = recv_dir / staged_name
    recv_final = recv_dir / filename

    # Step 1: Compress to local staging file
    t_comp0 = time.perf_counter()
    if codec == "gzip1":
        with stage_path.open("wb") as out_f:
            p1 = subprocess.run(
                ["gzip", "-1", "-c", str(src_path)],
                stdout=out_f,
                stderr=subprocess.PIPE,
                text=True,
            )
            if p1.returncode != 0:
                raise RuntimeError(f"gzip -1 failed: {p1.stderr}")
    elif codec == "zstd":
        p1 = subprocess.run(
            ["zstd", "-q", "-f", "-o", str(stage_path), str(src_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if p1.returncode != 0:
            raise RuntimeError(f"zstd compress failed: {p1.stderr}")
    else:
        raise ValueError(f"Unknown codec: {codec}")
    t_comp1 = time.perf_counter()

    staged_bytes = stage_path.stat().st_size

    # Step 2: Transfer compressed artifact via plain-TCP rsync
    dest_url = f"rsync://{env.srv_ip}:{env.rsync_port}/recv/{staged_name}"
    t_tx0 = time.perf_counter()
    p2 = subprocess.run(
        ["rsync", "--whole-file", "--no-compress", "--no-motd", "--quiet", str(stage_path), dest_url],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    t_tx1 = time.perf_counter()
    if p2.returncode != 0:
        raise RuntimeError(f"e2e rsync transfer failed: {p2.stderr}")

    # Step 3: Decompress on receiver, fsync final output, remove compressed temp
    t_dec0 = time.perf_counter()
    if codec == "gzip1":
        with recv_final.open("wb") as out_f:
            p3 = subprocess.run(
                ["gzip", "-dc", str(recv_staged)],
                stdout=out_f,
                stderr=subprocess.PIPE,
                text=True,
            )
            if p3.returncode != 0:
                raise RuntimeError(f"gzip -dc failed: {p3.stderr}")
    else:
        p3 = subprocess.run(
            ["zstd", "-d", "-q", "-f", "-o", str(recv_final), str(recv_staged)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if p3.returncode != 0:
            raise RuntimeError(f"zstd -d failed: {p3.stderr}")
    fsync_file(recv_final)
    recv_staged.unlink(missing_ok=True)
    t_dec1 = time.perf_counter()

    stage_path.unlink(missing_ok=True)

    compress_s = t_comp1 - t_comp0
    transfer_s = t_tx1 - t_tx0
    decompress_s = t_dec1 - t_dec0
    total_s = compress_s + transfer_s + decompress_s
    return total_s, compress_s, transfer_s, decompress_s, staged_bytes


def get_tool_version(cmd: list[str]) -> str:
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
        first = (res.stdout or "").strip().splitlines()
        return first[0] if first else "unknown"
    except OSError:
        return "unknown"


def collect_system_meta() -> dict:
    mem_total_kb = 0
    try:
        for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("MemTotal:"):
                mem_total_kb = int(line.split()[1])
                break
    except OSError:
        pass
    return {
        "hostname": platform.node(),
        "kernel": platform.release(),
        "arch": platform.machine(),
        "cpu_count": os.cpu_count(),
        "mem_gib": round(mem_total_kb / (1024 * 1024), 2),
        "gcc_version": get_tool_version(["gcc", "--version"]),
        "rsync_version": get_tool_version(["rsync", "--version"]),
        "zstd_version": get_tool_version(["zstd", "--version"]),
        "gzip_version": get_tool_version(["gzip", "--version"]),
    }


def summarize_runs(rows: list[dict], manifest: dict, args: argparse.Namespace) -> dict:
    grouped: dict[tuple[str, str, str], list[dict]] = {}
    for row in rows:
        if int(row["is_warmup"]) != 0:
            continue
        key = (row["profile"], row["file"], row["method"])
        grouped.setdefault(key, []).append(row)

    results: dict[str, dict] = {}
    for (profile, filename, method), group in grouped.items():
        totals = [float(r["total_s"]) for r in group]
        txs = [int(r["wire_tx_bytes"]) for r in group]
        rxs = [int(r["wire_rx_bytes"]) for r in group]
        file_bytes = int(group[0]["file_bytes"])

        med_total = statistics.median(totals)
        mean_total = statistics.mean(totals)
        std_total = statistics.stdev(totals) if len(totals) > 1 else 0.0
        med_tx = statistics.median(txs)
        med_rx = statistics.median(rxs)
        throughput_mibs = (file_bytes / (1024 * 1024)) / med_total if med_total > 0 else 0.0
        wire_ratio = med_tx / file_bytes if file_bytes > 0 else 0.0

        entry = {
            "runs": len(group),
            "file_bytes": file_bytes,
            "median_total_s": round(med_total, 4),
            "mean_total_s": round(mean_total, 4),
            "stddev_total_s": round(std_total, 4),
            "min_total_s": round(min(totals), 4),
            "max_total_s": round(max(totals), 4),
            "median_wire_tx_bytes": int(med_tx),
            "median_wire_rx_bytes": int(med_rx),
            "throughput_mibs": round(throughput_mibs, 2),
            "wire_ratio": round(wire_ratio, 4),
        }

        if method.startswith("e2e_"):
            comps = [float(r["compress_s"]) for r in group]
            xfers = [float(r["transfer_s"]) for r in group]
            decs = [float(r["decompress_s"]) for r in group]
            staged = [int(r["staged_bytes"]) for r in group]
            entry["median_compress_s"] = round(statistics.median(comps), 4)
            entry["median_transfer_s"] = round(statistics.median(xfers), 4)
            entry["median_decompress_s"] = round(statistics.median(decs), 4)
            entry["staged_bytes"] = int(statistics.median(staged))

        results.setdefault(profile, {}).setdefault(filename, {})[method] = entry

    return {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "config": {
            "size_mb": args.size_mb,
            "warmup_runs": args.warmup,
            "measured_runs": args.runs,
            "profiles": args.profiles,
            "files": args.files,
        },
        "system": collect_system_meta(),
        "manifest": manifest,
        "results": results,
    }


def methods_for_file(filename: str) -> list[str]:
    if filename in INCOMPRESSIBLE_FILES:
        return BASE_METHODS + INCOMPRESSIBLE_EXTRA_METHODS
    return list(BASE_METHODS)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run fct vs rsync benchmark suite.")
    parser.add_argument("--size-mb", type=int, default=256, help="File size in MiB (default: 256)")
    parser.add_argument("--runs", type=int, default=3, help="Measured runs per case (default: 3)")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup runs per case (default: 1)")
    parser.add_argument("--profiles", nargs="+", default=DEFAULT_PROFILES, choices=list(PROFILES.keys()))
    parser.add_argument("--files", nargs="+", default=DEFAULT_FILES, choices=DEFAULT_FILES)
    parser.add_argument("--output-dir", type=Path, default=Path("bench/results"))
    parser.add_argument("--data-dir", type=Path, default=Path("bench/data"))
    parser.add_argument("--skip-generate", action="store_true", help="Use existing manifest without regenerating")
    args = parser.parse_args()

    fct_bin = (ROOT_DIR / "build" / "fct").resolve()
    if not fct_bin.exists():
        print(f"Error: {fct_bin} not found. Run 'make' first.", file=sys.stderr)
        return 1

    data_dir = args.data_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    if args.skip_generate:
        manifest_path = data_dir / "manifest.json"
        if not manifest_path.exists():
            print(f"Error: {manifest_path} not found with --skip-generate.", file=sys.stderr)
            return 1
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        manifest = generate_data.generate_all(data_dir, args.size_mb)

    tmp_root = (ROOT_DIR / "bench" / "tmp" / f"run_{os.getpid()}").resolve()
    recv_dir = tmp_root / "recv"
    stage_dir = tmp_root / "stage"
    daemon_dir = tmp_root / "daemon"
    recv_dir.mkdir(parents=True, exist_ok=True)
    stage_dir.mkdir(parents=True, exist_ok=True)
    daemon_dir.mkdir(parents=True, exist_ok=True)

    csv_path = output_dir / "raw_runs.csv"
    rows: list[dict] = []

    total_cases = sum(len(methods_for_file(f)) for f in args.files) * len(args.profiles)
    total_execs = total_cases * (args.warmup + args.runs)
    exec_counter = 0
    port_counter = 0

    try:
        with BenchNetEnv() as env, csv_path.open("w", newline="", encoding="utf-8") as csv_f:
            writer = csv.DictWriter(csv_f, fieldnames=CSV_FIELDS)
            writer.writeheader()
            csv_f.flush()

            env.start_rsync_daemon(recv_dir, daemon_dir)

            for profile in args.profiles:
                env.apply_profile(profile)
                print(f"\n=== Profile: {profile} ===", flush=True)

                for filename in args.files:
                    file_meta = manifest["files"][filename]
                    src_path = Path(file_meta["path"]).resolve()
                    file_bytes = int(file_meta["size_bytes"])
                    expected_sha = file_meta["sha256"]

                    for method in methods_for_file(filename):
                        total_iters = args.warmup + args.runs
                        for it in range(total_iters):
                            is_warmup = 1 if it < args.warmup else 0
                            run_index = it if is_warmup else (it - args.warmup + 1)
                            exec_counter += 1

                            clean_dir(recv_dir)
                            clean_dir(stage_dir)

                            tx0, rx0 = env.read_veth_counters()

                            compress_s = ""
                            transfer_s = ""
                            decompress_s = ""
                            staged_bytes = ""

                            if method in ("fct_t1", "fct_t4", "fct_t1_z", "fct_t4_z"):
                                workers = 4 if "t4" in method else 1
                                force_z = method.endswith("_z")
                                port = 19100 + (port_counter % 2000)
                                port_counter += 1
                                total_s = run_fct(
                                    env, fct_bin, src_path, recv_dir, port, workers, force_z
                                )
                            elif method in ("rsync_raw", "rsync_z_default", "rsync_z_zlib"):
                                total_s = run_rsync(env, src_path, filename, method)
                            elif method == "e2e_gzip1":
                                total_s, cs, xs, ds, sb = run_e2e_staged(
                                    env, src_path, stage_dir, recv_dir, filename, "gzip1"
                                )
                                compress_s = f"{cs:.6f}"
                                transfer_s = f"{xs:.6f}"
                                decompress_s = f"{ds:.6f}"
                                staged_bytes = str(sb)
                            elif method == "e2e_zstd":
                                total_s, cs, xs, ds, sb = run_e2e_staged(
                                    env, src_path, stage_dir, recv_dir, filename, "zstd"
                                )
                                compress_s = f"{cs:.6f}"
                                transfer_s = f"{xs:.6f}"
                                decompress_s = f"{ds:.6f}"
                                staged_bytes = str(sb)
                            else:
                                raise ValueError(f"Unsupported method: {method}")

                            tx1, rx1 = env.read_veth_counters()
                            wire_tx = tx1 - tx0
                            wire_rx = rx1 - rx0

                            recv_file = recv_dir / filename
                            if not recv_file.exists():
                                raise RuntimeError(f"Missing output file {recv_file} after {method}")
                            actual_sha = generate_data.sha256_file(recv_file)
                            sha_ok = actual_sha == expected_sha
                            if not sha_ok:
                                raise RuntimeError(
                                    f"SHA-256 mismatch for {filename} ({method}): "
                                    f"expected {expected_sha}, got {actual_sha}"
                                )

                            clean_dir(recv_dir)
                            clean_dir(stage_dir)

                            throughput = (file_bytes / (1024 * 1024)) / total_s if total_s > 0 else 0.0
                            wire_ratio = wire_tx / file_bytes if file_bytes > 0 else 0.0

                            row = {
                                "profile": profile,
                                "file": filename,
                                "file_bytes": file_bytes,
                                "method": method,
                                "run_index": run_index,
                                "is_warmup": is_warmup,
                                "total_s": f"{total_s:.6f}",
                                "compress_s": compress_s,
                                "transfer_s": transfer_s,
                                "decompress_s": decompress_s,
                                "staged_bytes": staged_bytes,
                                "wire_tx_bytes": wire_tx,
                                "wire_rx_bytes": wire_rx,
                                "throughput_mibs": f"{throughput:.3f}",
                                "wire_ratio": f"{wire_ratio:.4f}",
                                "sha256_ok": "1" if sha_ok else "0",
                            }
                            rows.append(row)
                            writer.writerow(row)
                            csv_f.flush()

                            tag = "WARMUP" if is_warmup else f"RUN {run_index}/{args.runs}"
                            print(
                                f"[{exec_counter:3d}/{total_execs}] {profile:9s} | {filename:16s} | "
                                f"{method:15s} | {tag:7s} | {total_s:7.3f}s | "
                                f"{throughput:7.2f} MiB/s | wire={wire_tx / (1024*1024):6.2f} MiB ({wire_ratio:.2f}x)",
                                flush=True,
                            )
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

    summary = summarize_runs(rows, manifest, args)
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    generate_report.generate_report(output_dir)
    print(f"\n[done] Results written to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
