import copy
import json
from pathlib import Path

CONFIG_PATH = Path(__file__).with_name("config.json")

with CONFIG_PATH.open("r", encoding="utf-8") as f:
    config = json.load(f)

existing_cases = config.get("cases", [])
if not existing_cases:
    raise SystemExit("No existing case found to use as a command template.")

template = copy.deepcopy(existing_cases[0])
command_template = copy.deepcopy(template["command"])

cases = []


def add_case(
    experiment,
    protocol,
    window,
    size,
    loss=0,
    duplication=0,
    corruption=0,
    reordering=0,
    delay=0,
    jitter=0,
    reordering_delay=0,
    seed=101,
    rto=200,
):
    case = copy.deepcopy(template)
    case.update({
        "run_id": (
            f"{experiment}_{protocol.replace('-', '')}"
            f"_w{window}_l{loss}_d{duplication}"
            f"_c{corruption}_o{reordering}"
            f"_delay{delay}_jitter{jitter}"
            f"_reorddelay{reordering_delay}_seed{seed}"
        ),
        "experiment": experiment,
        "protocol": protocol,
        "window_size": window,
        "file_size_bytes": size,
        "loss_pct": loss,
        "duplication_pct": duplication,
        "corruption_pct": corruption,
        "reordering_pct": reordering,
        "delay_ms": delay,
        "jitter_ms": jitter,
        "reordering_delay_ms": reordering_delay,
        "seed": seed,
        "rto_ms": rto,
        "timeout_seconds": 180,
        "command": copy.deepcopy(command_template),
    })
    cases.append(case)


protocols = [
    ("stop-and-wait", 1),
    ("go-back-n", 4),
    ("selective-repeat", 4),
]

seeds = (101, 202, 303)

# A. Baseline: compare all three protocols.
for protocol, window in protocols:
    for seed in seeds:
        add_case(
            "baseline", protocol, window, 65536,
            seed=seed,
        )

# B. Packet-loss sensitivity: 1%, 5%, and 10%.
for protocol, window in protocols:
    for loss in (1, 5, 10):
        for seed in seeds:
            add_case(
                "loss", protocol, window, 65536,
                loss=loss, seed=seed,
            )

# C. Window-size scaling for Go-Back-N and Selective Repeat.
for protocol in ("go-back-n", "selective-repeat"):
    for window in (1, 4, 8, 16):
        for seed in seeds:
            add_case(
                "window", protocol, window, 262144,
                seed=seed,
            )

# D. Isolated reordering sensitivity.
for protocol, window in protocols:
    for seed in seeds:
        add_case(
            "reordering", protocol, window, 65536,
            reordering=5,
            reordering_delay=20,
            seed=seed,
        )

# E. Isolated packet duplication sensitivity.
for protocol, window in protocols:
    for seed in seeds:
        add_case(
            "duplication", protocol, window, 65536,
            duplication=2,
            seed=seed,
        )

# F. Isolated packet corruption sensitivity.
for protocol, window in protocols:
    for seed in seeds:
        add_case(
            "corruption", protocol, window, 65536,
            corruption=1,
            seed=seed,
        )

# G. Combined impairments.
for protocol, window in protocols:
    for seed in seeds:
        add_case(
            "combined", protocol, window, 65536,
            loss=5,
            duplication=2,
            corruption=1,
            reordering=5,
            delay=50,
            jitter=5,
            reordering_delay=20,
            seed=seed,
        )

# Update the configuration with a fresh, reproducible campaign.
config["output_csv"] = "raw/transfer_campaign_final.csv"
config["timeout_seconds"] = 180
config["cases"] = cases

with CONFIG_PATH.open("w", encoding="utf-8") as f:
    json.dump(config, f, indent=2)
    f.write("\n")

from collections import Counter

counts = Counter(case["experiment"] for case in cases)

print(f"Generated {len(cases)} experiment cases in {CONFIG_PATH}")
for experiment, count in sorted(counts.items()):
    print(f"  {experiment}: {count}")