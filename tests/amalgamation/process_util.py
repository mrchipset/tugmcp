#!/usr/bin/env python3
"""Bounded subprocess helpers with whole-process-tree cleanup.

Both amalgamation runners must never leak cmake/npx/fixture-server children
when a step times out. On Windows the tree is killed with ``taskkill /T /F``;
on POSIX children run in their own session so ``killpg`` reaches the group.
"""

from __future__ import annotations

import os
import signal
import subprocess
from pathlib import Path
from typing import Mapping, Optional, Sequence


def kill_process_tree(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        try:
            subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=15,
            )
        except (OSError, subprocess.TimeoutExpired):
            pass
    else:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        try:
            process.kill()
        except OSError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def run_bounded(
    command: Sequence[str],
    cwd: Path | None = None,
    timeout: int = 1800,
    capture_output: bool = False,
    env: Optional[Mapping[str, str]] = None,
) -> subprocess.CompletedProcess:
    """Run a command with a hard timeout and process-tree kill on expiry."""
    command = [str(part) for part in command]
    options = {
        "cwd": str(cwd) if cwd is not None else None,
        "env": dict(env) if env is not None else None,
        "stdout": subprocess.PIPE if capture_output else None,
        "stderr": subprocess.PIPE if capture_output else None,
        "text": True,
        "encoding": "utf-8",
        "errors": "replace",
    }
    if os.name != "nt":
        options["start_new_session"] = True
    process = subprocess.Popen(command, **options)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        kill_process_tree(process)
        try:
            stdout, stderr = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            stdout, stderr = "", ""
        raise subprocess.TimeoutExpired(
            command, timeout, output=stdout, stderr=stderr
        ) from None
    return subprocess.CompletedProcess(
        command, process.returncode, stdout, stderr
    )
