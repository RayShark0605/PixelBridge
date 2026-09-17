# PixelBridge documentation

[简体中文](README.md) | **English**

## Users

1. [Project overview](../README.en.md): use cases and quick start.
2. [User guide](USER_GUIDE.en.md): receiver-first, selected/whole/single-screen use, modes, stop and resume.
3. [Memory budgets](DECODER_MEMORY_BUDGET.en.md): configuration, recommendations and bounds.
4. [Diagnostics](DIAGNOSTICS.en.md): activity reasons, endpoint counters, errors and log files.
5. [Current status](PROJECT_STATUS.en.md): validated/unvalidated scope and v1.0 release boundaries.

## Developers

- [Architecture and technical route](ARCHITECTURE.en.md): complete current design, not merely an English abstract; PAM4/Wide, FEC/carousel, recovery, safety, capture lifetime and performance truth.
- [PAM4 contract](EXPERIMENTAL_PAM4_PROFILE.en.md) / [Wide contract](EXPERIMENTAL_PAM4_WIDE_PROFILE.en.md): exact parameters, frozen mappings, calibration/FEC, Goldens and compatibility identities.
- [Runtime option inventory](CURRENT_RUNTIME_OPTION_INVENTORY.md): audited adapter option names, not universal permission to combine options.
- [Build and contribution guide](../CONTRIBUTING.md), [AGENTS](../AGENTS.md).
- [PBBridge](REMOTE_OPS_BRIDGE.md): development orchestration only, no payload/ACK bypass.

## Shared normative and historical references

These retain exact module/field tables and historical evidence. They are not competing current roadmaps. Current behavior is documented above and verified against code/tests.

| Reference | Responsibility |
| --- | --- |
| [Descriptor Schema](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | Byte fields, lengths, reserved values, CRC and conflict rules |
| [Raster](REFERENCE_RASTER.md) | Pixel-reference definitions |
| [Sender persistence](ENCODER_STREAMING_CAROUSEL.md) | Persistent state/leases and historical scheduler evidence |
| [Receiver persistence](DECODER_RESUMABLE_RECOVERY.md) | PBJH/PBJR, checkpoints, compaction and publish crash windows |
| [Presentation](PRESENTATION.md) | Data windows, sampling and Present lifecycle |
| [Regions](SCREEN_REGION.md), [WGC](PBScreenCaptureWgc.md) | Physical pixels, DPI, textures and frame leases |
| [Telemetry](UNIFIED_TELEMETRY_REPORT.md) | RunReport and verified-goodput contracts |
| [Goldens](GOLDEN_VECTOR_HARNESS.md) | Deterministic regression and evidence boundaries |
| [Evidence](EVIDENCE_INDEX.md) | Current retained results, failures and waivers |
| [Document history](DOC_HISTORY.md) | Retired paths, working-copy hashes/backups and committed-blob recovery |
| [Tools](../tools/README.md), [Fuzz](../fuzz/README.md) | Local research/test references, not default product features |

Local historical/tool documents are not all translated line by line. **The maintained user guide, architecture, modes, budgets, diagnostics and status have full English companions.** Machine field tables and Goldens are shared rather than duplicated into divergent wire definitions.

## Maintenance

- Keep user-facing README, current status, evidence and history separate.
- Mode labels: Standard / Gray Fast / PAM4 / PAM4 Wide. Frozen internal `experimental-*` tokens remain for compatibility.
- Preserve source/package/session/environment identities for every performance claim; old throughput does not automatically apply to new code.
- No payloads, recordings, private credentials/addresses, logs or build trees in Git.
- Protected local reports, AGENTS and `.zcode` are outside this cleanup.
- v1.0 uses MIT; see the bilingual [release notes](RELEASE_V1.0.md) for package contents and validation boundaries.
