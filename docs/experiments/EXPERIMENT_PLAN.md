# Reliable UDP experiment plan

## Purpose

Measure the implemented Stop-and-Wait (SW), Go-Back-N (GBN), and Selective Repeat (SR) protocols using the project's Driver and Channel Emulator. The benchmark is intended to describe this implementation under controlled conditions, not to claim superiority over TCP or another production transport.

## Rules for defensible measurements

- Run from one clean build/configuration and record the Git commit, compiler, OS, CPU, build flags, and date.
- Use fixed channel seeds per replicate, but use distinct seeds across replicates.
- Generate deterministic binary input files and verify the output byte-for-byte (and SHA-256 if available) after every successful transfer.
- Record failed runs as failures; do not silently discard them. Report completion rate alongside latency and goodput.
- Do not mix debug and release results. Use release builds for performance measurements; run correctness tests separately.
- Avoid event logging during timed performance runs because per-event file I/O can distort timings. Keep summary metrics enabled and record `monitor_output_failed` separately.
- Run one warm-up per scenario, label it as warm-up, and exclude it from measured replicates. Randomize scenario order where practical.
- Report median and interquartile range (IQR), plus all individual samples. Do not report only the best run.
- Keep the channel configuration, payload size, chunk size, protocol, window, RTO settings, retry/deadline limits, seed, and result status in every raw row.

## Experiment A — protocol behaviour as loss increases

**Question:** How do SW, GBN, and SR completion time, goodput, and retransmission cost change under increasing random packet loss?

- Payload: 64 KiB deterministic binary file
- Chunk: 1024 bytes
- One-way configured delay: 5 ms; jitter: 1 ms
- Bandwidth: 100 Mbps; queue limit: 1 MiB
- Loss probability: 0%, 1%, 3%, 5%
- Duplication/corruption/reordering: 0%
- Window: SW=1; GBN=8; SR=8
- Replicates: 5 measured seeds per cell, plus one warm-up per cell
- Outputs: completion rate, elapsed time, goodput, packets, retransmissions, timeouts, RTT/RTO metrics, integrity status

This compares each protocol's normal operating configuration. It is not a strictly equal-window comparison because SW necessarily has a window of one; interpret protocol and window effects together.

## Experiment B — window-size sweep

**Question:** How does the sender window affect throughput and retransmission overhead for pipelined protocols?

- Protocols: GBN and SR
- Window sizes: 1, 4, 8, 16, 32
- Payload: 1 MiB; chunk: 1024 bytes
- Delay: 5 ms; jitter: 0 ms; bandwidth: 100 Mbps
- Loss cells: 0% and 2%; all other impairment probabilities 0%
- Replicates: 5 measured seeds per cell, plus one warm-up per cell
- Outputs: same as Experiment A

Window=1 is retained as a useful reference. Do not describe the largest measured window as optimal unless the results show that it balances goodput and retransmission cost.

## Experiment C — file-size scaling

**Question:** How does transfer duration and goodput scale with payload size, and how much does a nonzero RTT change the protocol comparison?

- Payload sizes: 64 KiB, 1 MiB, 4 MiB
- Protocols: SW, GBN, SR; windows SW=1, GBN=8, SR=8
- Channel profiles: (1) no artificial delay/loss; (2) 5 ms one-way delay, no loss
- Chunk: 1024 bytes; bandwidth: 100 Mbps
- Replicates: 3 measured seeds per cell, plus one warm-up per cell
- Outputs: same as Experiment A

The 4 MiB SW case with delay is expected to be slower; use a generous transfer deadline and report timeouts as failures rather than shortening the deadline selectively.

## Experiment D — combined impairment / integrity stress

**Question:** Can transfers still complete with data integrity preserved when several channel impairments occur together?

- Payload: 256 KiB; chunk: 1024 bytes
- Protocols: SW, GBN, SR; windows SW=1, GBN=8, SR=8
- Delay: 5 ms; jitter: 2 ms; bandwidth: 100 Mbps
- Loss: 2%; duplication: 1%; corruption: 0.5%; reordering: 10%; reordering delay: 8 ms
- Replicates: 5 measured seeds per protocol, plus one warm-up per protocol
- Outputs: completion rate, integrity result, elapsed time, retransmissions, timeouts, duplicate/out-of-order counters, RTT/RTO metrics

This is a robustness experiment, not a clean throughput comparison: combined impairments make individual runs more variable.

## Derived metrics

- `goodput_bytes_per_second = verified_payload_bytes / elapsed_seconds` for successful runs only.
- `retransmission_ratio = retransmissions / data_packets_sent` (state this denominator explicitly).
- `completion_rate = successful_verified_runs / measured_runs` per cell.
- Summaries should include `n`, median, IQR, minimum, and maximum. Include failures separately; do not assign them a fabricated goodput value.

## Preconditions before performance runs

1. Run the full project build and all unit/integration tests on the exact commit being measured.
2. Confirm the updated Driver fixes are merged and exercised by the integration tests.
3. Confirm source and destination files differ only in path, not content; verify size and hash after each successful run.
4. Confirm the sender starts only after the receiver is listening.
5. Record the commit hash and complete configuration with the raw output.
