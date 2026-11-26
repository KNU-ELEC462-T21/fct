#!/usr/bin/env python3
"""Generate benchmark datasets for fct vs rsync comparisons."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import random
import subprocess
import sys
import tarfile
import time
import zlib
from pathlib import Path

DEFAULT_DATA_DIR = Path("bench/data")

METHODS = ("GET", "POST", "PUT", "DELETE", "PATCH")
PATHS = (
    "/api/v1/transfers",
    "/api/v1/chunks/upload",
    "/api/v1/sessions/status",
    "/api/v1/files/metadata",
    "/healthz",
    "/metrics",
)
STATUSES = (200, 200, 200, 201, 204, 304, 400, 404, 500, 503)
AGENTS = (
    "fct-cli/1.0",
    "Mozilla/5.0 (X11; Linux x86_64)",
    "curl/8.5.0",
    "python-requests/2.31.0",
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            buf = f.read(4 * 1024 * 1024)
            if not buf:
                break
            h.update(buf)
    return h.hexdigest()


def make_log_block(rng: random.Random, block_bytes: int, target_ratio_min: float = 0.12,
                   target_ratio_max: float = 0.22) -> bytes:
    """Build a reusable realistic log block whose gzip-1 ratio falls near 15-20%."""
    lines: list[bytes] = []
    size = 0
    seq = 0
    while size < block_bytes:
        ip = f"10.24.{rng.randint(1, 32)}.{rng.randint(1, 254)}"
        method = rng.choice(METHODS)
        path = rng.choice(PATHS)
        status = rng.choice(STATUSES)
        latency_us = rng.randint(100, 9999)
        bytes_sent = rng.randint(1024, 65536)
        agent = rng.choice(AGENTS)
        req_id = f"req-{seq // 4:06d}-{rng.randint(0, 255):02x}"
        ts = f"2026-11-26T12:{(seq // 3600) % 60:02d}:{(seq // 60) % 60:02d}.{seq % 1000:03d}Z"
        line = (
            f'{ts} level=INFO service=fct-gateway env=production region=ap-northeast-2 '
            f'ip={ip} method={method} path="{path}?id={seq % 200}" '
            f'status={status} latency_us={latency_us} bytes={bytes_sent} '
            f'req_id={req_id} ua="{agent}"\n'
        ).encode("ascii")
        lines.append(line)
        size += len(line)
        seq += 1
    data = b"".join(lines)[:block_bytes]
    ratio = len(zlib.compress(data[: min(len(data), 1024 * 1024)], level=1)) / min(
        len(data), 1024 * 1024
    )
    if not (target_ratio_min <= ratio <= target_ratio_max):
        print(
            f"[warn] log sample gzip-1 ratio={ratio:.3f} (outside {target_ratio_min}-{target_ratio_max})",
            file=sys.stderr,
        )
    return data


def generate_compressible_log(path: Path, target_bytes: int) -> None:
    rng = random.Random(4201)
    block_size = min(1024 * 1024, target_bytes)
    written = 0
    with path.open("wb") as f:
        while written < target_bytes:
            chunk_len = min(block_size, target_bytes - written)
            block = make_log_block(rng, chunk_len)
            f.write(block)
            written += len(block)


def make_non_magic_random(rng: random.Random, size: int) -> bytes:
    buf = bytearray(rng.randbytes(size))
    # Ensure no accidental ZIP/GZIP/MP4 header signature in first 8 bytes.
    if len(buf) >= 8:
        buf[0] = 0xAA
        buf[1] = 0x55
        buf[4:8] = b"\xDE\xAD\xBE\xEF"
    return bytes(buf)


def generate_mixed_tar(path: Path, target_bytes: int) -> None:
    """Create a valid tar archive with ~50-60% gzip-1 compressibility overall and in chunk 0."""
    rng = random.Random(4202)
    member_size = 256 * 1024  # 256 KiB members -> 8 members per 2 MiB chunk
    log_template = make_log_block(rng, member_size, target_ratio_min=0.10, target_ratio_max=0.22)

    written = 0
    idx = 0
    with tarfile.open(path, mode="w") as tf:
        while written + 1024 < target_bytes:
            remaining = target_bytes - written - 1024  # leave room for tar EOF blocks
            # Reserve 512 bytes for tar header
            payload_cap = remaining - 512
            if payload_cap <= 0:
                break
            payload_len = min(member_size, payload_cap)
            # Round down to 512-byte boundary unless final small piece
            if payload_len > 512:
                payload_len = (payload_len // 512) * 512

            if idx % 2 == 0:
                # Compressible text member
                payload = log_template[:payload_len]
                if len(payload) < payload_len:
                    payload = (log_template * ((payload_len // len(log_template)) + 1))[:payload_len]
                name = f"logs/segment_{idx:04d}.log"
            else:
                # Incompressible random member
                payload = make_non_magic_random(rng, payload_len)
                name = f"blobs/chunk_{idx:04d}.bin"

            info = tarfile.TarInfo(name=name)
            info.size = len(payload)
            info.mtime = 1700000000
            info.mode = 0o644
            tf.addfile(info, io.BytesIO(payload))
            padded = ((len(payload) + 511) // 512) * 512
            written += 512 + padded
            idx += 1

    # Pad to exact target_bytes if slightly short
    actual = path.stat().st_size
    if actual < target_bytes:
        with path.open("ab") as f:
            f.write(b"\x00" * (target_bytes - actual))


def generate_archive_gz(path: Path, target_bytes: int) -> None:
    """Create a valid .gz file whose compressed size is ~target_bytes."""
    rng = random.Random(4203)
    block_size = 1024 * 1024
    # Compress incompressible data at level 1 -> stored deflate blocks (~1.0003x expansion)
    comp = zlib.compressobj(level=1, method=zlib.DEFLATED, wbits=16 + zlib.MAX_WBITS)
    # Slightly trim input so final .gz size is close to target_bytes
    input_target = max(1, target_bytes - max(1024, (target_bytes // 65535) * 6 + 64))
    remaining = input_target
    with path.open("wb") as f:
        while remaining > 0:
            n = min(block_size, remaining)
            raw = rng.randbytes(n)
            f.write(comp.compress(raw))
            remaining -= n
        f.write(comp.flush())


def generate_random_bin(path: Path, target_bytes: int) -> None:
    rng = random.Random(4204)
    block_size = 1024 * 1024
    written = 0
    with path.open("wb") as f:
        while written < target_bytes:
            n = min(block_size, target_bytes - written)
            chunk = rng.randbytes(n)
            if written == 0 and len(chunk) >= 8:
                buf = bytearray(chunk)
                buf[0] = 0xAA
                buf[1] = 0x55
                buf[4:8] = b"\xDE\xAD\xBE\xEF"
                chunk = bytes(buf)
            f.write(chunk)
            written += n


def measure_compressed_size(cmd: list[str]) -> int:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return len(proc.stdout)


def verify_existing(manifest_path: Path, data_dir: Path, size_mb: int) -> bool:
    if not manifest_path.exists():
        return False
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    if manifest.get("size_mb") != size_mb:
        return False
    files = manifest.get("files", {})
    expected_names = ["compressible.log", "mixed.tar", "archive.gz", "random.bin"]
    if sorted(files.keys()) != sorted(expected_names):
        return False
    for name in expected_names:
        meta = files[name]
        p = data_dir / name
        if not p.exists() or p.stat().st_size != meta.get("size_bytes"):
            return False
        if sha256_file(p) != meta.get("sha256"):
            return False
    return True


def generate_all(data_dir: Path, size_mb: int, force: bool = False) -> dict:
    data_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = data_dir / "manifest.json"

    if not force and verify_existing(manifest_path, data_dir, size_mb):
        print(f"[data] Reusing verified {size_mb} MiB dataset in {data_dir}")
        return json.loads(manifest_path.read_text(encoding="utf-8"))

    target_bytes = size_mb * 1024 * 1024
    generators = [
        ("compressible.log", generate_compressible_log),
        ("mixed.tar", generate_mixed_tar),
        ("archive.gz", generate_archive_gz),
        ("random.bin", generate_random_bin),
    ]

    files_meta: dict[str, dict] = {}
    for name, fn in generators:
        out_path = data_dir / name
        t0 = time.perf_counter()
        print(f"[data] Generating {name} (~{size_mb} MiB)...", flush=True)
        fn(out_path, target_bytes)
        actual_bytes = out_path.stat().st_size
        digest = sha256_file(out_path)
        gzip1_bytes = measure_compressed_size(["gzip", "-1", "-c", str(out_path)])
        zstd_bytes = measure_compressed_size(["zstd", "-q", "-c", str(out_path)])

        # Also check first 2 MiB chunk gzip-1 ratio (mirrors fct adaptive trial)
        with out_path.open("rb") as f:
            first_chunk = f.read(2 * 1024 * 1024)
        first_chunk_gz1 = len(zlib.compress(first_chunk, level=1))
        first_chunk_ratio = first_chunk_gz1 / len(first_chunk) if first_chunk else 1.0

        dt = time.perf_counter() - t0
        files_meta[name] = {
            "filename": name,
            "path": str(out_path),
            "size_bytes": actual_bytes,
            "sha256": digest,
            "gzip1_bytes": gzip1_bytes,
            "gzip1_ratio": round(gzip1_bytes / actual_bytes, 4),
            "zstd_bytes": zstd_bytes,
            "zstd_ratio": round(zstd_bytes / actual_bytes, 4),
            "first_chunk_gzip1_ratio": round(first_chunk_ratio, 4),
        }
        print(
            f"[data] {name}: {actual_bytes / (1024 * 1024):.2f} MiB, "
            f"gzip-1={files_meta[name]['gzip1_ratio']:.1%}, "
            f"zstd={files_meta[name]['zstd_ratio']:.1%}, "
            f"chunk0_gz1={first_chunk_ratio:.1%} ({dt:.1f}s)"
        )

    manifest = {
        "size_mb": size_mb,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "files": files_meta,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate fct vs rsync benchmark datasets.")
    parser.add_argument("--size-mb", type=int, default=256, help="Target size per file in MiB (default: 256)")
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR, help="Output directory")
    parser.add_argument("--force", action="store_true", help="Regenerate even if manifest matches")
    args = parser.parse_args()

    if args.size_mb < 2:
        print("--size-mb must be >= 2", file=sys.stderr)
        return 1

    generate_all(args.data_dir, args.size_mb, force=args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
