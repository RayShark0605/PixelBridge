# PBRemoteVisualReport

`pb_remote_visual_report.py` strictly merges one independently exported Encoder report and one Decoder report. It rejects duplicate JSON keys, invalid UTF-8, non-finite values, oversized trees, role/RunId/profile/source/SessionTag/digest mismatches, RemoteVisual metadata whose embedded RunId differs from its endpoint report, and non-overlapping run windows.

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

A successful combined run requires Decoder `Completed`, WholeFileDigest PASS, final publish PASS, matching endpoint identity, and external source/output SHA-256 plus length equality. Encoder cycle position is never treated as receiver completion. The combined report always states `certifiedRemoteVisualProfile=false`.

The optional cross-run CSV includes provider/mode/geometry metadata, sender broadcast cadence, Capture/Unique/EndToEndUnique FPS, Bootstrap/BER/FER, RemoteVisual metric confidence split by verified versus rejected frames, duplicate/reorder/gap/stall counters, verified goodput, and final integrity results. `high-confidence-wrong` is never fabricated from production receive alone because that path has no independent per-codeword truth oracle; use sealed Replay evidence for that classification.

When clocks are not synchronized, supply the measured Decoder-to-Encoder offset and uncertainty with `--decoder-clock-offset-ms` and `--clock-uncertainty-ms`. Do not guess these values for formal evidence.
