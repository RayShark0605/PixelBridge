# PBRemoteVisualReport

`pb_remote_visual_report.py` strictly merges one independently exported Encoder report and one Decoder report. It rejects duplicate JSON keys, invalid UTF-8, non-finite values, oversized trees, role/RunId/profile/source/SessionId/SessionTag/digest mismatches, RemoteVisual metadata whose embedded RunId differs from its endpoint report, and non-overlapping run windows. For `PB-RemoteVisual-LF4-X1`, it additionally pins profile/layout/data/codeword constants and validates every FEC, signal-metric, unreliable-symbol and freshness numerator against its declared denominator and `null` semantics.

Example (use the repository-required Python):

```powershell
D:\Python3.12.9\python.exe tools\PBRemoteVisualReport\pb_remote_visual_report.py `
  --encoder D:\evidence\encoder-run-0123456789abcdef0123456789abcdef.json `
  --decoder D:\evidence\decoder-run-0123456789abcdef0123456789abcdef.json `
  --source-file D:\evidence\source.bin `
  --published-file D:\evidence\published.bin `
  --output-dir D:\evidence `
  --summary-csv D:\evidence\remote-runs.csv
```

A successful combined run requires Decoder `Completed`, WholeFileDigest PASS, final publish PASS, matching endpoint identity, complete endpoint journals, exact create-time seals for **both original endpoint report files**, and external source/output SHA-256 plus length equality. Supplying equivalent in-memory dictionaries without both report artifacts can produce a diagnostic merge, but never `successfulRun=true`. Encoder cycle position is never treated as receiver completion. The combined report always states `certifiedRemoteVisualProfile=false`.

Every invocation publishes three create-only per-run artifacts:

- `remote-run-<RunId>-combined.json`
- `remote-run-<RunId>.md`
- `remote-run-<RunId>.csv`

The optional `--summary-csv` remains an append-only cross-run index. It is not a substitute for the create-only per-run CSV.

Both CSV forms keep the theoretical profile ceiling, actually generated payload rate, presented cadence, captured/unique cadence, raw FEC evaluation, Receiver-bound temporal admission and verified goodput in separate columns. They also include provider/mode/geometry metadata, Bootstrap/BER/FER, RemoteVisual metric confidence split by verified versus rejected frames, unreliable-symbol and freshness erasure rates, duplicate/reorder/gap/stall counters, verified goodput, and final integrity results. `high-confidence-wrong` is never fabricated from production receive alone because that path has no independent per-codeword truth oracle; use sealed Replay evidence for that classification.

When clocks are not synchronized, supply the measured Decoder-to-Encoder offset and uncertainty with `--decoder-clock-offset-ms` and `--clock-uncertainty-ms`. Do not guess these values for formal evidence.
