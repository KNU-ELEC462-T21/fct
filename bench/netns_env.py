#!/usr/bin/env python3
"""Manage isolated Linux network namespace, veth pair, tc shaping, and rsync daemon."""

from __future__ import annotations

import getpass
import os
import signal
import socket
import subprocess
import time
from pathlib import Path


NS_NAME = "fct_bench_srv"
CLI_VETH = "veth_fct_cli"
SRV_VETH = "veth_fct_srv"
CLI_IP = "10.200.77.1"
SRV_IP = "10.200.77.2"
CIDR_LEN = 24
RSYNC_PORT = 18730

PROFILES = {
    "wan_100m": {"rate": "100mbit", "delay": "10ms", "limit": "4000"},
    "lan_1g": {"rate": "1000mbit", "delay": "1ms", "limit": "4000"},
    "unlimited": None,
}


def _run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=check)


class BenchNetEnv:
    """Isolated client(host) <-> server(netns) veth testbed."""

    def __init__(self) -> None:
        self.user = getpass.getuser()
        self.cli_ip = CLI_IP
        self.srv_ip = SRV_IP
        self.rsync_port = RSYNC_PORT
        self._rsync_proc: subprocess.Popen | None = None
        self._rsync_conf: Path | None = None
        self._rsync_pid: Path | None = None
        self._prev_sigint = None
        self._prev_sigterm = None
        self._active = False

    def __enter__(self) -> "BenchNetEnv":
        self._prev_sigint = signal.getsignal(signal.SIGINT)
        self._prev_sigterm = signal.getsignal(signal.SIGTERM)
        signal.signal(signal.SIGINT, self._handle_signal)
        signal.signal(signal.SIGTERM, self._handle_signal)
        self.cleanup()
        self.setup()
        self._active = True
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        try:
            self.cleanup()
        finally:
            self._active = False
            if self._prev_sigint is not None:
                signal.signal(signal.SIGINT, self._prev_sigint)
            if self._prev_sigterm is not None:
                signal.signal(signal.SIGTERM, self._prev_sigterm)

    def _handle_signal(self, signum: int, frame) -> None:
        self.cleanup()
        raise SystemExit(128 + signum)

    def setup(self) -> None:
        _run(["sudo", "ip", "netns", "add", NS_NAME])
        _run(["sudo", "ip", "link", "add", CLI_VETH, "type", "veth", "peer", "name", SRV_VETH])
        _run(["sudo", "ip", "link", "set", SRV_VETH, "netns", NS_NAME])

        _run(["sudo", "ip", "addr", "add", f"{CLI_IP}/{CIDR_LEN}", "dev", CLI_VETH])
        _run(["sudo", "ip", "link", "set", CLI_VETH, "mtu", "1500", "up"])

        _run(["sudo", "ip", "netns", "exec", NS_NAME, "ip", "link", "set", "lo", "up"])
        _run(["sudo", "ip", "netns", "exec", NS_NAME, "ip", "addr", "add", f"{SRV_IP}/{CIDR_LEN}", "dev", SRV_VETH])
        _run(["sudo", "ip", "netns", "exec", NS_NAME, "ip", "link", "set", SRV_VETH, "mtu", "1500", "up"])

        # Sanity ping across veth pair
        _run(["ping", "-c", "1", "-W", "2", SRV_IP])

    def cleanup(self) -> None:
        self.stop_rsync_daemon()
        _run(["sudo", "ip", "link", "del", CLI_VETH], check=False)
        _run(["sudo", "ip", "netns", "del", NS_NAME], check=False)

    def apply_profile(self, profile: str) -> None:
        if profile not in PROFILES:
            raise ValueError(f"Unknown network profile: {profile}")
        spec = PROFILES[profile]
        if spec is None:
            _run(["sudo", "tc", "qdisc", "del", "dev", CLI_VETH, "root"], check=False)
            _run(["sudo", "ip", "netns", "exec", NS_NAME, "tc", "qdisc", "del", "dev", SRV_VETH, "root"], check=False)
        else:
            args = [
                "root",
                "netem",
                "delay",
                spec["delay"],
                "rate",
                spec["rate"],
                "limit",
                spec["limit"],
            ]
            _run(["sudo", "tc", "qdisc", "replace", "dev", CLI_VETH, *args])
            _run(["sudo", "ip", "netns", "exec", NS_NAME, "tc", "qdisc", "replace", "dev", SRV_VETH, *args])

    def read_veth_counters(self) -> tuple[int, int]:
        tx_path = Path(f"/sys/class/net/{CLI_VETH}/statistics/tx_bytes")
        rx_path = Path(f"/sys/class/net/{CLI_VETH}/statistics/rx_bytes")
        return int(tx_path.read_text().strip()), int(rx_path.read_text().strip())

    def server_cmd_prefix(self) -> list[str]:
        return ["sudo", "ip", "netns", "exec", NS_NAME, "sudo", "-u", self.user]

    def start_server_popen(self, cmd: list[str], **kwargs) -> subprocess.Popen:
        full_cmd = [*self.server_cmd_prefix(), *cmd]
        return subprocess.Popen(full_cmd, **kwargs)

    def start_rsync_daemon(self, recv_dir: Path, tmp_dir: Path, port: int = RSYNC_PORT) -> None:
        self.stop_rsync_daemon()
        recv_dir.mkdir(parents=True, exist_ok=True)
        tmp_dir.mkdir(parents=True, exist_ok=True)
        conf_path = tmp_dir / "rsyncd.conf"
        pid_path = tmp_dir / "rsyncd.pid"
        lock_path = tmp_dir / "rsyncd.lock"
        log_path = tmp_dir / "rsyncd.log"
        if pid_path.exists():
            pid_path.unlink()

        conf_path.write_text(
            f"use chroot = no\n"
            f"pid file = {pid_path.resolve()}\n"
            f"lock file = {lock_path.resolve()}\n"
            f"log file = {log_path.resolve()}\n"
            f"port = {port}\n"
            f"address = {SRV_IP}\n"
            f"max connections = 8\n"
            f"timeout = 600\n\n"
            f"[recv]\n"
            f"path = {recv_dir.resolve()}\n"
            f"read only = no\n",
            encoding="utf-8",
        )
        self._rsync_conf = conf_path
        self._rsync_pid = pid_path

        cmd = [
            "rsync",
            "--daemon",
            "--no-detach",
            f"--config={conf_path.resolve()}",
        ]
        self._rsync_proc = self.start_server_popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        # Wait until TCP port accepts connections
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self._rsync_proc.poll() is not None:
                stderr_out = self._rsync_proc.stderr.read() if self._rsync_proc.stderr else ""
                raise RuntimeError(f"rsync daemon exited prematurely: {stderr_out}")
            try:
                with socket.create_connection((SRV_IP, port), timeout=0.5):
                    return
            except OSError:
                time.sleep(0.05)

        raise TimeoutError(f"Timed out waiting for rsync daemon on {SRV_IP}:{port}")

    def stop_rsync_daemon(self) -> None:
        if self._rsync_proc is not None:
            proc = self._rsync_proc
            self._rsync_proc = None
            if proc.poll() is None:
                # Kill process inside netns using pid file if present, plus Popen
                if self._rsync_pid and self._rsync_pid.exists():
                    try:
                        pid = int(self._rsync_pid.read_text().strip())
                        _run(["sudo", "kill", "-TERM", str(pid)], check=False)
                    except (ValueError, OSError):
                        pass
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    if self._rsync_pid and self._rsync_pid.exists():
                        try:
                            pid = int(self._rsync_pid.read_text().strip())
                            _run(["sudo", "kill", "-KILL", str(pid)], check=False)
                        except (ValueError, OSError):
                            pass
                    proc.kill()
                    proc.wait(timeout=3)
            if proc.stderr is not None:
                proc.stderr.close()

        if self._rsync_pid and self._rsync_pid.exists():
            self._rsync_pid.unlink(missing_ok=True)
        if self._rsync_conf and self._rsync_conf.exists():
            self._rsync_conf.unlink(missing_ok=True)
        self._rsync_pid = None
        self._rsync_conf = None
