#!/usr/bin/env python3
"""Analyze a Reliable-UDP transfer campaign CSV and generate summaries/plots."""
import argparse, json, os
import pandas as pd
import matplotlib.pyplot as plt

EXPECTED = {"baseline": 9, "loss": 27, "window": 24, "reordering": 9,
            "duplication": 9, "corruption": 9, "combined": 9}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", help="Path to transfer_campaign_final.csv")
    ap.add_argument("--out", default="results/analysis", help="Output directory")
    args = ap.parse_args()
    df = pd.read_csv(args.csv)
    os.makedirs(os.path.join(args.out, "summaries"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "plots"), exist_ok=True)
    required = ["run_id","experiment","protocol","status","integrity_ok",
                "exit_code","goodput_bytes_per_second","elapsed_seconds",
                "packets_sent","retransmissions","timeouts","seed","window_size","loss_pct"]
    missing_cols = [c for c in required if c not in df.columns]
    if missing_cols:
        raise SystemExit("Missing required columns: " + ", ".join(missing_cols))
    df["integrity_ok"] = df["integrity_ok"].astype(str).str.lower().map(
        {"true": True, "false": False})
    for c in ["goodput_bytes_per_second","elapsed_seconds","packets_sent",
              "retransmissions","timeouts","exit_code","seed","window_size","loss_pct"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")

    exp_counts = {str(k): int(v) for k,v in df.experiment.value_counts().items()}
    validation = {
        "input_file": os.path.basename(args.csv), "rows": int(len(df)),
        "expected_rows": 96, "rows_match_expected": len(df) == 96,
        "duplicate_run_ids": int(df.run_id.duplicated().sum()),
        "missing_cells": int(df.isna().sum().sum()),
        "status_counts": {str(k): int(v) for k,v in df.status.value_counts(dropna=False).items()},
        "integrity_true": int((df.integrity_ok == True).sum()),
        "integrity_false_or_missing": int((df.integrity_ok != True).sum()),
        "exit_code_counts": {str(k): int(v) for k,v in df.exit_code.value_counts(dropna=False).items()},
        "experiment_counts": exp_counts, "expected_experiment_counts": EXPECTED,
        "experiment_counts_match": {k: int(df.experiment.value_counts().get(k,0)) == v
                                    for k,v in EXPECTED.items()},
        "seed_counts": {str(k): int(v) for k,v in df.seed.value_counts().items()},
    }
    with open(os.path.join(args.out,"summaries","validation.json"),"w") as f:
        json.dump(validation,f,indent=2)

    df.groupby(["experiment","protocol"], dropna=False).agg(
        runs=("run_id","count"),
        completed=("status",lambda s:(s=="completed").sum()),
        integrity_verified=("integrity_ok","sum"),
        goodput_median=("goodput_bytes_per_second","median"),
        goodput_mean=("goodput_bytes_per_second","mean"),
        goodput_sd=("goodput_bytes_per_second","std"),
        elapsed_median_s=("elapsed_seconds","median"),
        packets_sent_median=("packets_sent","median"),
        retransmissions_median=("retransmissions","median"),
        timeouts_median=("timeouts","median")
    ).reset_index().to_csv(os.path.join(args.out,"summaries","summary_by_experiment_protocol.csv"),index=False)

    df.groupby(["loss_pct","protocol"]).agg(
        runs=("run_id","count"), goodput_median=("goodput_bytes_per_second","median"),
        goodput_mean=("goodput_bytes_per_second","mean"), goodput_sd=("goodput_bytes_per_second","std"),
        retransmissions_median=("retransmissions","median"),timeouts_median=("timeouts","median")
    ).reset_index().to_csv(os.path.join(args.out,"summaries","loss_sensitivity.csv"),index=False)

    df[df.experiment=="window"].groupby(["window_size","protocol"]).agg(
        runs=("run_id","count"),goodput_median=("goodput_bytes_per_second","median"),
        goodput_mean=("goodput_bytes_per_second","mean"),goodput_sd=("goodput_bytes_per_second","std"),
        elapsed_median_s=("elapsed_seconds","median")
    ).reset_index().to_csv(os.path.join(args.out,"summaries","window_sweep.csv"),index=False)

    df.groupby(["experiment","protocol"]).agg(
        runs=("run_id","count"),retransmissions_median=("retransmissions","median"),
        timeouts_median=("timeouts","median"),packets_sent_median=("packets_sent","median")
    ).reset_index().to_csv(os.path.join(args.out,"summaries","recovery_metrics.csv"),index=False)

    plots = os.path.join(args.out,"plots")
    for exp in ["baseline","corruption","duplication","reordering","combined"]:
        sub=df[df.experiment==exp]
        stats=sub.groupby("protocol").goodput_bytes_per_second.agg(["mean","std"]).reindex(
            ["stop-and-wait","go-back-n","selective-repeat"])
        if stats.empty: continue
        stats["std"]=stats["std"].fillna(0)
        fig,ax=plt.subplots(figsize=(8,4.8))
        ax.bar(stats.index,stats["mean"],yerr=stats["std"],capsize=4)
        ax.set_ylabel("Goodput (bytes/s)")
        ax.set_title(f"{exp.title()}: mean goodput ± SD (n=3/protocol)")
        ax.tick_params(axis="x",rotation=12); ax.grid(axis="y",alpha=.25)
        fig.tight_layout(); fig.savefig(os.path.join(plots,f"{exp}_goodput.png"),dpi=180); plt.close(fig)

    sub=df[df.experiment=="loss"]
    fig,ax=plt.subplots(figsize=(8,4.8))
    for proto in ["stop-and-wait","go-back-n","selective-repeat"]:
        stats=sub[sub.protocol==proto].groupby("loss_pct").goodput_bytes_per_second.agg(["mean","std"])
        ax.errorbar(stats.index,stats["mean"],yerr=stats["std"],marker="o",capsize=4,label=proto)
    ax.set_xlabel("Configured packet loss (%)"); ax.set_ylabel("Goodput (bytes/s)")
    ax.set_title("Loss sensitivity: mean goodput ± SD (n=3/point)")
    ax.legend(); ax.grid(alpha=.25); fig.tight_layout()
    fig.savefig(os.path.join(plots,"loss_sensitivity.png"),dpi=180); plt.close(fig)

    sub=df[df.experiment=="window"]
    fig,ax=plt.subplots(figsize=(8,4.8))
    for proto in ["go-back-n","selective-repeat"]:
        stats=sub[sub.protocol==proto].groupby("window_size").goodput_bytes_per_second.agg(["mean","std"])
        ax.errorbar(stats.index,stats["mean"],yerr=stats["std"],marker="o",capsize=4,label=proto)
    ax.set_xlabel("Window size"); ax.set_ylabel("Goodput (bytes/s)")
    ax.set_title("Window sweep: mean goodput ± SD (n=3/point)")
    ax.legend(); ax.grid(alpha=.25); fig.tight_layout()
    fig.savefig(os.path.join(plots,"window_sweep.png"),dpi=180); plt.close(fig)
    print(json.dumps(validation,indent=2))

if __name__ == "__main__":
    main()
