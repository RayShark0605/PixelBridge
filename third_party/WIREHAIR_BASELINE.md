# Wirehair dependency baseline

| Field | Pinned value |
| --- | --- |
| Upstream | `catid/wirehair` |
| Project version | `2.0.0` |
| Revision | `067ca7cdb66aed424ec23f97557429bf791c6f0c` |
| Source archive SHA-512 | `dc0267b3f441df6b417bb4ecb80d8adbbbc1e44fe235a4f00b9856f2350f9133d3ca325967064b6ecb7bbb35b181ce20a9685f99846d98e74ab86d93a5c3a07a` |
| License | BSD-3-Clause |
| CMake target | `wirehair::wirehair` (static only) |

Authoritative pinned artifacts:

- [exact upstream commit](https://github.com/catid/wirehair/commit/067ca7cdb66aed424ec23f97557429bf791c6f0c)
- [canonical V2 wire-profile specification](https://github.com/catid/wirehair/blob/067ca7cdb66aed424ec23f97557429bf791c6f0c/V2_WIRE_PROFILE.md)
- [serialized V2 public API](https://github.com/catid/wirehair/blob/067ca7cdb66aed424ec23f97557429bf791c6f0c/include/wirehair/wirehair.h)
- [BSD-3-Clause license](https://github.com/catid/wirehair/blob/067ca7cdb66aed424ec23f97557429bf791c6f0c/LICENSE)

The overlay forces `BUILD_SHARED_LIBS=OFF` and disables `BUILD_TESTS`,
`BUILD_CODEC_V2`, `MARCH_NATIVE`, `WIREHAIR_BUILD_BOTH`, tools, benchmarks,
scheduled tests, libFuzzer, LTO, PGO, and strict third-party warnings. The public
serialized V2 implementation remains part of the main upstream library even
with `BUILD_CODEC_V2=OFF`.

PixelBridge uses only the canonical serialized-profile V2 boundary. A profile
ID identifies a frozen equation family and is not an integrity, trust, or
sender-authentication primitive. The third-party revision is an implementation
baseline and does not change PixelBridge's wire-protocol version.

Receiver admission and adversarial-ID protection also depend on implementation
properties of this exact pinned revision: the V2 decoder retains at most
`K + 1024` accepted IDs, initially reserves `K + 32` receive rows, and its
received-ID table has an upstream `sizeof(ReceivedPacketRecord) == 24`
assertion and uses a per-decoder salt with the pinned 64-bit mixer. The salt
varies bucket placement so a remote sender cannot precompute the backend's
linear-probe clusters. PBOuterFec's admission estimate charges all 24 bytes for
every slot in that private table.

PBOuterFec does not copy or claim to mirror this private bucket placement. It
maintains a separate bounded payload-fingerprint table with its own per-decoder
OS-CSPRNG salt and a 64-probe local limit. The wrapper table protects duplicate
and conflict accounting; the backend's independently salted table and accepted
ID ceiling remain separate defenses. Neither hash is wire behavior or sender
authentication. Any Wirehair revision change therefore requires re-auditing
the accepted-ID ceiling, table allocation/reservation formula, and backend hash
hardening, then rerunning the quota, collision, fuzz, benchmark, and Golden
Vector gates.
