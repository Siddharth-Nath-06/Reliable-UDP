# Reliable-UDP Experiment Plan

## Purpose

Evaluate the implemented Stop-and-Wait (SW), Go-Back-N (GBN), and Selective Repeat (SR) protocols using the project's C experiment runner and channel emulator. The campaign describes this implementation under the configured conditions; it does not establish that a protocol is universally superior or comparable to a production transport.

This document describes the **currently configured final campaign**. It should be revised after the run only if the actual configuration or methodology changes. Results and conclusions must be based on the generated CSV, not predicted in advance.

## Final campaign at a glance

The case generator creates **96 cases**, with three configured seeds (`101`, `202`, `303`) for each configuration.

| Experiment group | Configurations | Cases | Payload size |
|---|---|---:|---:|
| Baseline protocol comparison | SW, GBN, SR; no configured impairments | 9 | 65,536 bytes |
| Packet-loss sensitivity | SW, GBN, SR; loss 1%, 5%, 10% | 27 | 65,536 bytes |
| Window-size sweep | GBN and SR; windows 1, 4, 8, 16 | 24 | 262,144 bytes |
| Reordering sensitivity | SW, GBN, SR; reordering 5%, reordering delay 20 ms | 9 | 65,536 bytes |
| Duplication sensitivity | SW, GBN, SR; duplication 2% | 9 | 65,536 bytes |
| Corruption sensitivity | SW, GBN, SR; corruption 1% | 9 | 65,536 bytes |
| Combined impairments | SW, GBN, SR; loss, duplication, corruption, reordering, delay, and jitter | 9 | 65,536 bytes |
| **Total** | **7 groups, 3 seeds per configuration** | **96** | — |

The case count is `9 + 27 + 24 + 9 + 9 + 9 + 9 = 96`.

## Shared run settings

- Protocols: Stop-and-Wait, Go-Back-N, and Selective Repeat.
- Seeds: `101`, `202`, and `303` for each configuration.
- Retransmission timeout: `200 ms`.
- Per-case timeout: `180 seconds`.
- Results CSV: `results/raw/transfer_campaign_final.csv`.
- Output is one row per configured case, including failures and timeouts.
- The experiment runner records process status, exit code, elapsed time, standard output/error excerpts, and metrics emitted by the C runner when those metrics match the CSV schema.
- The configured window is 1 for Stop-and-Wait; the baseline/loss/impairment cases use window 4 for GBN and SR. The window sweep varies GBN and SR across 1, 4, 8, and 16.

The C harness currently uses its own configured packet/chunk behavior. Do not assume a chunk size, bandwidth limit, queue limit, or symmetric impairment in both directions unless verified from the implementation and recorded for this campaign.

## Experiment 1 — Baseline protocol comparison

**Question:** Under the baseline configuration, how do elapsed time, goodput, retransmissions, and other available metrics compare across the three protocols?

- Payload: 65,536 bytes.
- Protocol/window pairs: SW/1, GBN/4, SR/4.
- Configured impairments: zero loss, duplication, corruption, reordering, delay, and jitter.
- Seeds: 101, 202, 303.

This is a comparison of the selected protocol configurations, not a claim that all protocol parameters are otherwise identical.

## Experiment 2 — Packet-loss sensitivity

**Question:** How do the protocols behave as configured packet loss increases?

- Payload: 65,536 bytes.
- Protocol/window pairs: SW/1, GBN/4, SR/4.
- Loss rates: 1%, 5%, and 10%.
- Other configured impairments: zero.
- Seeds: 101, 202, 303 per protocol/loss combination.

Compare goodput and elapsed time for successful, integrity-verified transfers, and report failures and retransmission/timeout behavior separately.

## Experiment 3 — Window-size sweep

**Question:** How does sender window size affect GBN and SR performance?

- Payload: 262,144 bytes.
- Protocols: GBN and SR.
- Window sizes: 1, 4, 8, 16.
- Configured impairments: zero.
- Seeds: 101, 202, 303 per protocol/window combination.

Do not describe the largest window as optimal unless the measurements support that conclusion and the retransmission/failure costs are considered.

## Experiment 4 — Reordering sensitivity

**Question:** How do the protocols behave when packet reordering is enabled?

- Payload: 65,536 bytes.
- Protocol/window pairs: SW/1, GBN/4, SR/4.
- Reordering: 5%.
- Reordering delay: 20 ms.
- Other configured impairments: zero.
- Seeds: 101, 202, 303.

## Experiment 5 — Duplication sensitivity

**Question:** Do duplicate packets affect successful completion, integrity, or protocol overhead?

- Payload: 65,536 bytes.
- Protocol/window pairs: SW/1, GBN/4, SR/4.
- Duplication: 2%.
- Other configured impairments: zero.
- Seeds: 101, 202, 303.

## Experiment 6 — Corruption sensitivity

**Question:** How do the protocols handle configured packet corruption?

- Payload: 65,536 bytes.
- Protocol/window pairs: SW/1, GBN/4, SR/4.
- Corruption: 1%.
- Other configured impairments: zero.
- Seeds: 101, 202, 303.

## Experiment 7 — Combined impairments

**Question:** Can transfers complete with integrity preserved when multiple impairments are configured together?

- Payload: 65,536 bytes.
- Protocol/window pairs: SW/1, GBN/4, SR/4.
- Loss: 5%.
- Duplication: 2%.
- Corruption: 1%.
- Reordering: 5%.
- Configured delay: 50 ms.
- Jitter: 5 ms.
- Reordering delay: 20 ms.
- Seeds: 101, 202, 303.

Treat this as a robustness test. Its results should not be conflated with the isolated one-factor experiments.

## Metrics and interpretation

The CSV schema includes the following fields, although some may be blank if the runner does not emit them:

- `status`, `exit_code`, `elapsed_seconds`, and `integrity_ok`
- `goodput_bytes_per_second`
- `packets_sent`, `retransmissions`, and `timeouts`
- protocol, window, payload size, impairment settings, seed, and RTO
- captured `stdout` and `stderr` excerpts

Analysis rules:

1. Count `completed`, `failed`, `timeout`, and `launch_error` outcomes separately.
2. Treat a transfer as successful only when the process status and the integrity/file-verification result both support success.
3. Compute goodput summaries only from successful, integrity-verified runs. Do not invent goodput for failed runs.
4. Report the sample count (`n`) and distribution, preferably median and interquartile range (IQR); include individual observations or a per-run CSV for traceability. Means and standard deviations may be added when useful.
5. Report completion rate as successful verified runs divided by all configured runs in the relevant cell. State the denominator.
6. If calculating a retransmission ratio, define the denominator explicitly and ensure the required packet count is available.
7. Do not infer results from the configured settings alone. Explain conclusions only after examining the raw data.

## Reproducibility and limitations

Before or alongside the campaign, record the Git commit, date, OS, compiler/toolchain, build flags, and relevant machine details. Preserve the configuration used for the run with the raw CSV.

The three seeds provide repeated observations, but they are a limited sample. The campaign is not a full factorial study: most experiments vary one impairment at a time, while the combined group changes several factors. Channel-emulator behavior is implementation-specific; confirm from the code which transport path is impaired before making claims about directionality.

## Running the campaign

From the repository root in PowerShell:

```powershell
python .\experiments\run_experiments.py --dry-run --limit 2
```

The dry run prints commands but does not execute cases. The runner reserves the configured output CSV, so remove that empty file before a real run if it was created by a dry run.

After a successful smoke test, preserve the smoke-test CSV under a different name and run the full campaign with a fresh output path:

```powershell
Rename-Item .\results\raw\transfer_campaign_final.csv transfer_campaign_smoke.csv
python .\experiments\run_experiments.py
```

Do not rename an existing full campaign over the smoke-test file; use a unique backup name if the file already exists.
