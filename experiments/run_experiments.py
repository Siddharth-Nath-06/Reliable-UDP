#!/usr/bin/env python3
"""
Config-driven experiment runner.

Run from any directory; expects config.json next to this script.
Each case runs one complete transfer and emits a JSON object on stdout
containing metrics, where available.
"""

import argparse
import csv
import datetime as dt
import hashlib
import json
import pathlib
import shlex
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
RESULTS = ROOT / "results"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--config",
        default=str(HERE / "config.json"),
        help="Path to experiment configuration JSON",
    )
    ap.add_argument(
        "--precheck",
        action="store_true",
        help="Run the PowerShell build/test prechecker first",
    )
    ap.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Run only the first N cases (smoke test)",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without executing them",
    )
    args = ap.parse_args()

    config_path = pathlib.Path(args.config).resolve()
    cfg = json.loads(config_path.read_text(encoding="utf-8"))

    RESULTS.mkdir(parents=True, exist_ok=True)

    if args.precheck:
        script = HERE / "precheck.ps1"
        cmd = [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
        ]

        if cfg.get("build_command"):
            cmd += ["-BuildCommand", cfg["build_command"]]

        if cfg.get("test_command"):
            cmd += ["-TestCommand", cfg["test_command"]]

        subprocess.run(cmd, cwd=ROOT, check=True)

    cases = cfg.get("cases", [])

    if args.limit:
        if args.limit < 1:
            raise SystemExit("--limit must be a positive integer.")
        cases = cases[:args.limit]

    if not cases:
        raise SystemExit(
            "No cases configured. Edit experiments/config.json."
        )

    output_csv = pathlib.Path(
        cfg.get("output_csv", "transfer_results.csv")
    )

    if output_csv.is_absolute():
        out = output_csv
    else:
        out = RESULTS / output_csv

    out.parent.mkdir(parents=True, exist_ok=True)

    fields = [
        "run_id",
        "experiment",
        "protocol",
        "window_size",
        "file_size_bytes",
        "loss_pct",
        "duplication_pct",
        "corruption_pct",
        "reordering_pct",
        "delay_ms",
        "jitter_ms",
        "reordering_delay_ms",
        "seed",
        "rto_ms",
        "status",
        "elapsed_seconds",
        "goodput_bytes_per_second",
        "packets_sent",
        "retransmissions",
        "timeouts",
        "integrity_ok",
        "exit_code",
        "stdout",
        "stderr",
    ]

    # Never silently mix a new campaign with existing results.
    if out.exists() and out.stat().st_size > 0:
        raise SystemExit(
            f"Output already exists: {out}\n"
            "Move or rename the existing file before starting "
            "a fresh campaign."
        )

    with out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=fields,
            extrasaction="ignore",
        )
        writer.writeheader()
        f.flush()

        for idx, case in enumerate(cases, 1):
            run_id = case.get(
                "run_id",
                f"{dt.datetime.now():%Y%m%d_%H%M%S}_{idx:04d}",
            )

            context = {
                key: str(value)
                for key, value in case.items()
                if not isinstance(value, (dict, list))
            }
            context["run_id"] = run_id

            cmd_template = case.get("command")

            if not cmd_template:
                raise SystemExit(
                    f"Case {run_id} has no 'command'. "
                    "Configure it to match your driver CLI."
                )

            try:
                cmd = [
                    str(part).format(**context)
                    for part in cmd_template
                ]
            except KeyError as e:
                raise SystemExit(
                    f"Case {run_id} has an unresolved command "
                    f"placeholder: {e}"
                )

            print(
                f"[{idx}/{len(cases)}] {run_id}: "
                f"{shlex.join(cmd)}",
                flush=True,
            )

            row = {key: case.get(key, "") for key in fields}
            row.update(
                run_id=run_id,
                experiment=case.get("experiment", ""),
                status="not_run",
            )

            if args.dry_run:
                print("  dry-run: not executed", flush=True)
                continue

            start = time.perf_counter()

            try:
                process = subprocess.run(
                    cmd,
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=float(
                        case.get(
                            "timeout_seconds",
                            cfg.get("timeout_seconds", 180),
                        )
                    ),
                    encoding="utf-8",
                    errors="replace",
                )

                elapsed = time.perf_counter() - start

                row.update(
                    elapsed_seconds=f"{elapsed:.6f}",
                    exit_code=process.returncode,
                    stdout=process.stdout.strip()[-4000:],
                    stderr=process.stderr.strip()[-2000:],
                )

                row["status"] = (
                    "completed"
                    if process.returncode == 0
                    else "failed"
                )

                # Prefer metrics from the last stdout line that is
                # a valid JSON object.
                for line in reversed(process.stdout.splitlines()):
                    try:
                        metrics = json.loads(line)
                    except (ValueError, TypeError):
                        continue

                    if isinstance(metrics, dict):
                        row.update({
                            key: value
                            for key, value in metrics.items()
                            if key in fields
                        })
                        break

                if row.get("goodput_bytes_per_second") in ("", None):
                    size = int(case.get("file_size_bytes", 0) or 0)

                    if (
                        size
                        and elapsed > 0
                        and row["status"] == "completed"
                    ):
                        row["goodput_bytes_per_second"] = (
                            f"{size / elapsed:.3f}"
                        )

                print(
                    f"  {row['status']} ({elapsed:.2f}s)",
                    flush=True,
                )

            except subprocess.TimeoutExpired as e:
                elapsed = time.perf_counter() - start

                stdout = e.stdout or ""
                stderr = e.stderr or ""

                if isinstance(stdout, bytes):
                    stdout = stdout.decode("utf-8", errors="replace")

                if isinstance(stderr, bytes):
                    stderr = stderr.decode("utf-8", errors="replace")

                row.update(
                    status="timeout",
                    elapsed_seconds=f"{elapsed:.6f}",
                    stdout=stdout[-4000:],
                    stderr=stderr[-2000:],
                )

                print("  TIMEOUT", flush=True)

            except OSError as e:
                row.update(
                    status="launch_error",
                    elapsed_seconds=f"{time.perf_counter() - start:.6f}",
                    stderr=str(e),
                )

                print(f"  LAUNCH ERROR: {e}", flush=True)

            writer.writerow(row)
            f.flush()

    if args.dry_run:
        print(
            f"\nDry run complete: {len(cases)} commands inspected."
        )
        print("No experiment commands were executed.")
        print(f"Output CSV reserved: {out}")
    else:
        print(f"\nResults written to: {out}")
        print(
            "Check failed, timeout, and launch_error rows, "
            "as well as integrity_ok, before analyzing results."
        )


if __name__ == "__main__":
    main()