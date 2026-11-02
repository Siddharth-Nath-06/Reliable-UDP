# Reliable-UDP build, test, and experiment automation

## Current state

The repository provides a GCC/Make build, unit-test targets, and a C-based experiment runner at `experiments/experiment_runner.c`.

The runner performs one complete sender/receiver transfer, checks the resulting files, and prints a final JSON metrics line. The Python script `experiments/run_experiments.py` reads cases from `experiments/config.json`, executes the configured command for each case, parses the JSON metrics, and appends a row to the configured CSV file under `results/`.

The Stop-and-Wait smoke case has been run successfully with a 65,536-byte file, no configured packet loss, and seed `101`. The sender and receiver reported success, the files matched, integrity was verified, and the runner produced positive goodput. This confirms the basic harness path; it does not yet validate all protocols or impairment settings.

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

Or ask the experiment runner to perform the precheck and then print the first configured case without executing it:

```powershell
python .\experiments\run_experiments.py --precheck --dry-run --limit 1
```

A failed build or test should block experiments. Investigate test failures rather than weakening tests to make the precheck pass.

## Run the C experiment harness directly

Build the runner:

```powershell
make experiment-runner
```

Example: a 65,536-byte Stop-and-Wait transfer with no configured loss and a fixed seed:

```powershell
.\build\experiment_runner.exe --protocol stop-and-wait --window 1 --size 65536 --loss 0 --seed 101
```

The runner reports sender/receiver status and file equality, followed by a JSON summary containing metrics such as packets sent, retransmissions, timeouts, integrity status, goodput, and exit code. Treat a transfer as successful only when the process result and integrity/file-equality checks both indicate success.

## Run configured experiments

The experiment cases and output filename are configured in `experiments/config.json`. The current smoke case invokes `build/experiment_runner.exe` with the protocol, window size, file size, loss setting, and seed.

Run the first configured case:

```powershell
python .\experiments\run_experiments.py --limit 1
```

Run all configured cases (after reviewing the configuration):

```powershell
python .\experiments\run_experiments.py
```

Preview commands without executing them:

```powershell
python .\experiments\run_experiments.py --dry-run --limit 1
```

The runner appends results to the CSV named by `output_csv` in `experiments/config.json`, under `results/`. Use a new output filename when changing the CSV schema; the script does not migrate an existing CSV header. For example, the current smoke-test results can be written to `results/transfer_results_v2.csv` to avoid mixing the newer schema with an older results file.

## Interpreting results

- Inspect `status`, `exit_code`, and `integrity_ok` before analyzing performance.
- Do not count failed, launch-error, or timed-out runs as successful transfers.
- Check the received-file integrity result; an exit code of zero alone is not enough.
- Use a fixed random seed when comparing runs so the configured channel randomness is reproducible.
- Compare repeated runs and record the protocol, window size, file size, channel settings, and seed.
- Goodput and packet metrics are only meaningful if the transfer and integrity checks succeeded.
- The channel-emulator configuration is applied on the sender's transport path in the current harness. Interpret loss and other impairment results with that scope in mind; do not assume both directions are impaired symmetrically.

## Recommended validation before a large benchmark batch

1. Run the loss-free smoke test and confirm the sender and receiver succeed, the files match, integrity is true, and goodput is positive.
2. Add smoke cases for Go-Back-N and Selective Repeat, using protocol-valid window sizes.
3. Test a larger file without impairments.
4. Test a fixed-seed loss case and confirm that retransmission and timeout metrics behave plausibly while the transfer still passes integrity checks.
5. Only then expand the experiment matrix and analyze performance.

## Results and repository hygiene

- Experiment output is appended; repeated runs accumulate rows.
- Preserve previous result files when changing schemas or experiment configurations.
- Review failed and timed-out rows rather than silently discarding them.
- Avoid committing generated binaries or temporary transfer files unless the repository intentionally tracks them.
