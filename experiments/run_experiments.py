#!/usr/bin/env python3
"""
Config-driven experiment runner.
Run from any directory; expects config.json next to this script.
Each case is a command that performs ONE complete transfer/run and emits a JSON
object on stdout (or writes a JSON result file as configured). No CLI assumptions
about Reliable-UDP are made: command templates must match your actual driver.
"""
import argparse, csv, datetime as dt, hashlib, json, os, pathlib, shlex, subprocess, sys, time

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
    ap.add_argument("--config", default=str(HERE / "config.json"))
    ap.add_argument("--precheck", action="store_true", help="run the PowerShell build/test prechecker first")
    ap.add_argument("--limit", type=int, default=0, help="only run first N cases (smoke test)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    cfg = json.loads(pathlib.Path(args.config).read_text(encoding="utf-8"))
    RESULTS.mkdir(exist_ok=True)
    if args.precheck:
        script = HERE / "precheck.ps1"
        cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)]
        if cfg.get("build_command"): cmd += ["-BuildCommand", cfg["build_command"]]
        if cfg.get("test_command"): cmd += ["-TestCommand", cfg["test_command"]]
        subprocess.run(cmd, cwd=ROOT, check=True)

    cases = cfg.get("cases", [])
    if args.limit:
        cases = cases[:args.limit]
    if not cases:
        raise SystemExit("No cases configured. Edit experiments/config.json.")
    out = RESULTS / cfg.get("output_csv", "transfer_results.csv")
    fields = ["run_id","experiment","protocol","window_size","file_size_bytes","loss_pct",
              "duplication_pct","corruption_pct","reordering_pct","delay_ms","jitter_ms",
              "seed","rto_ms","status","elapsed_seconds","goodput_bytes_per_second",
              "packets_sent","retransmissions","timeouts","integrity_ok","exit_code","stdout","stderr"]
    existing = out.exists() and out.stat().st_size > 0
    with out.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        if not existing: writer.writeheader()
        for idx, case in enumerate(cases, 1):
            run_id = case.get("run_id", f"{dt.datetime.now():%Y%m%d_%H%M%S}_{idx:04d}")
            context = {k: str(v) for k,v in case.items() if not isinstance(v, (dict,list))}
            context["run_id"] = run_id
            cmd_template = case.get("command")
            if not cmd_template:
                raise SystemExit(f"Case {run_id} has no 'command'. Configure it to match your driver CLI.")
            # Use a list of argument strings, avoiding shell parsing where possible.
            cmd = [str(part).format(**context) for part in cmd_template]
            print(f"[{idx}/{len(cases)}] {run_id}: {' '.join(shlex.quote(x) for x in cmd)}")
            row = {k: case.get(k, "") for k in fields}
            row.update(run_id=run_id, experiment=case.get("experiment",""), status="not_run")
            if args.dry_run:
                print("  dry-run: not executed")
                continue
            start = time.perf_counter()
            try:
                p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                                   timeout=float(case.get("timeout_seconds", cfg.get("timeout_seconds", 180))),
                                   encoding="utf-8", errors="replace")
                elapsed = time.perf_counter() - start
                row.update(elapsed_seconds=f"{elapsed:.6f}", exit_code=p.returncode,
                           stdout=p.stdout.strip()[-4000:], stderr=p.stderr.strip()[-2000:])
                row["status"] = "completed" if p.returncode == 0 else "failed"
                # Preferred contract: last stdout line is a JSON object with metrics.
                for line in reversed(p.stdout.splitlines()):
                    try:
                        metrics = json.loads(line)
                        if isinstance(metrics, dict):
                            row.update({k:v for k,v in metrics.items() if k in fields})
                            break
                    except (ValueError, TypeError):
                        pass
                if row.get("goodput_bytes_per_second") in ("", None):
                    size = int(case.get("file_size_bytes", 0) or 0)
                    if size and elapsed > 0 and row["status"] == "completed":
                        row["goodput_bytes_per_second"] = f"{size/elapsed:.3f}"
                print(f"  {row['status']} ({elapsed:.2f}s)")
            except subprocess.TimeoutExpired as e:
                row.update(status="timeout", elapsed_seconds=f"{time.perf_counter()-start:.6f}",
                           stdout=(e.stdout or "")[-4000:] if isinstance(e.stdout,str) else "",
                           stderr=(e.stderr or "")[-2000:] if isinstance(e.stderr,str) else "")
                print("  TIMEOUT")
            except OSError as e:
                row.update(status="launch_error", stderr=str(e))
                print(f"  LAUNCH ERROR: {e}")
            writer.writerow(row); f.flush()
    print(f"\nResults appended to: {out}")
    print("Check failed/timeout rows and integrity_ok before analyzing. Do not treat failed runs as successful transfers.")

if __name__ == "__main__":
    main()
