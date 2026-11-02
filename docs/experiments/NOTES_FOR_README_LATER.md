# README follow-up notes

Update the repository README after the final experiment campaign has completed and the raw data has been validated.

## Suggested links

- `docs/experiments/EXPERIMENT_PLAN.md` — the configured experiment groups, parameters, metrics, and interpretation rules.
- `docs/experiments/AUTOMATION_README.md` — build, test, smoke-test, and campaign commands.
- `results/raw/transfer_campaign_final.csv` — raw per-run results from the final campaign, only after the campaign actually completes and the file is validated.
- `results/analysis/` — generated summary CSVs and plots, once the analysis pipeline has created them.
- `results/validation_run.log` and `results/validation_summary.csv` — validation evidence, if these files are present and correspond to the commit being documented.

## Publication rules

- Do not describe the campaign as complete until the full configured run has finished.
- Do not link an empty, partial, or smoke-test CSV as the final measured dataset.
- Keep smoke-test data separate from final-campaign data.
- Record the Git commit, date, operating system, compiler/toolchain, build flags, and relevant machine details for each campaign.
- Report failed, timed-out, and launch-error cases; do not silently discard them.
- Link only files and directories that actually exist in the repository. If the analysis outputs are not generated yet, label them as planned rather than completed.
- Avoid writing benchmark conclusions into the README until the raw CSV has been checked and the analysis has been regenerated from it.
