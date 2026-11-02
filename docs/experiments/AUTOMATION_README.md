# Reliable-UDP build, test, and experiment automation

## Current state

The repository provides a GCC/Make build, test targets, a C experiment runner at `experiments/experiment_runner.c`, and a Python campaign runner at `experiments/run_experiments.py`.

The C runner performs a sender/receiver transfer, checks the resulting files, and emits a final JSON metrics line. The Python runner reads cases from `experiments/config.json`, executes the configured command for each case, parses available JSON metrics, and writes one CSV row per executed case.

The configured final campaign contains 96 cases across seven groups: baseline protocol comparison, packet-loss sensitivity, window-size sweep, reordering sensitivity, duplication sensitivity, corruption sensitivity, and combined impairments. The intended output is `results/raw/transfer_campaign_final.csv`.

The Stop-and-Wait smoke test has passed for a 65,536-byte file with no configured packet loss, using seeds `101` and `202`. Both runs completed, reported integrity as `True`, and returned exit code `0`. This validates the basic harness path for those two baseline runs only; it does not by itself validate all protocols or impairment settings.

## Prerequisites

- GCC from MSYS2 UCRT64, available in `PATH`
- GNU Make, available in `PATH`
- Python 3, available in `PATH`
- Windows PowerShell for `precheck.ps1`

Run commands from the repository root in PowerShell or an MSYS2 UCRT64 terminal where `gcc` and `make` resolve correctly.

## Build and unit tests

```powershell
make clean
make all
make test
```

Run the PowerShell prechecker directly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\experiments\precheck.ps1
```

Or run the precheck and preview the first configured case:

```powershell
python .\experiments\run_experiments.py --precheck --dry-run --limit 1
```

A failed build or test should block the campaign. Investigate test failures rather than weakening tests to make the precheck pass.

## Build and run the C experiment harness directly

Build the runner:

```powershell
make experiment-runner
```

Example: a 65,536-byte Stop-and-Wait transfer with no configured loss and a fixed seed:

```powershell
.\build\experiment_runner.exe --protocol stop-and-wait --window 1 --size 65536 --loss 0 --seed 101
```

The runner reports transfer status and file equality, followed by JSON metrics such as packets sent, retransmissions, timeouts, integrity status, goodput, and exit code. Treat a transfer as successful only when both the process result and integrity/file-verification checks support success.

## Configured campaign

The cases and output filename are defined in `experiments/config.json`. The current case generator creates 96 cases, with seeds `101`, `202`, and `303` per configuration. The campaign covers:

- Baseline comparison of SW, GBN, and SR
- Loss rates of 1%, 5%, and 10%
- GBN/SR window sizes 1, 4, 8, and 16
- Isolated reordering, duplication, and corruption
- A combined-impairment robustness group

See `docs/experiments/EXPERIMENT_PLAN.md` for the detailed matrix and interpretation rules.

### Syntax check and dry run

From the repository root:

```powershell
python -m py_compile .\experiments\run_experiments.py
python .\experiments\run_experiments.py --dry-run --limit 2
```

The dry run prints the commands without executing them. **The current runner reserves the output CSV path before processing cases**, so a dry run can leave an empty CSV file. Remove that empty file before starting a real campaign.

### Smoke test

A real smoke test can be run using the first two configured cases:

```powershell
python .\experiments\run_experiments.py --limit 2
```

The runner refuses to reuse a non-empty output CSV. Preserve the smoke-test file under a separate name before starting the full campaign. For example, if the smoke-test CSV is at the configured final path:

```powershell
Rename-Item .\results\raw\transfer_campaign_final.csv transfer_campaign_smoke.csv
```

Then start the full campaign:

```powershell
python .\experiments\run_experiments.py
```

Do not run the rename command if the destination `transfer_campaign_smoke.csv` already exists; choose another unique backup name instead. If the configured output CSV already exists and is non-empty, the runner stops rather than appending new results to it.

### Dry-run caution

The runner's dry-run mode does not execute experiment commands or write result rows, but it does reserve the output file by opening it and writing the CSV header. Therefore, after a dry run, remove the empty/header-only output file before a real run. Do not remove a file containing real results.

## Results CSV

The configured output is `results/raw/transfer_campaign_final.csv`. Its columns include case parameters, status, elapsed time, goodput, packet/retransmission/timeout metrics, integrity status, exit code, and captured output. The schema includes `reordering_delay_ms`.

The runner creates parent directories for the output path. It does not overwrite a non-empty output file; preserve previous data and choose a fresh path for a new campaign. Inspect the CSV header and row count before analysis.

## Interpreting results

- Inspect `status`, `exit_code`, and `integrity_ok` before analyzing performance.
- Report failed, timed-out, and launch-error cases; do not count them as successful transfers.
- An exit code of zero alone is not enough: check the integrity/file-verification result too.
- Goodput and packet metrics should only be used as successful-transfer performance measurements when integrity verification passed.
- Compare repeated seeds and retain protocol, window size, file size, channel settings, and seed alongside each measurement.
- The configured impairment settings describe the emulator; do not assume they affect both directions symmetrically unless confirmed in the implementation.
- Treat the campaign as evidence about this implementation and these configured conditions, not as a universal ranking of reliable transport protocols.

## After the campaign

1. Confirm that the full run finished and that the CSV contains the expected 96 rows plus one header row.
2. Check status counts, exit codes, integrity results, and missing metrics.
3. Preserve the raw CSV unchanged.
4. Generate summary tables and plots from the raw data with a reproducible analysis script.
5. Record the commit and execution environment.
6. Update the README with links and conclusions only after the data has been validated and analyzed.

## Repository hygiene

- Keep raw campaign data separate from smoke-test output.
- Preserve previous result files when changing experiment configurations or schemas.
- Review failures instead of silently discarding them.
- Avoid committing generated binaries or temporary transfer files unless the repository intentionally tracks them.
