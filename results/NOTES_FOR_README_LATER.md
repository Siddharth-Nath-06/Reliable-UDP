# README follow-up notes

When the results are final, link these artifacts from the repository README:

- `results/EXPERIMENT_PLAN.md` — experimental questions, controls, metrics, and run rules
- `results/transfer_results.csv` — raw per-run data (currently header-only; populate only from actual runs)
- `results/summary_*.csv` — per-cell aggregates (create after the raw results exist)
- `results/validation_run.log` and `results/validation_summary.csv` — module validation evidence
- `results/plots/` — plots generated from raw CSV data, with axis units and error bars / distributions

Do not link or describe the empty transfer-results template as measured benchmark results. Record the exact Git commit and environment for every benchmark batch.
