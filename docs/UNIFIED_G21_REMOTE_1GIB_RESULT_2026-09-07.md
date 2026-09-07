# G21 真实远控 15 Hz / 1 GiB 终态与单次验收豁免（2026-09-07）

## 1. 最终结论

本次冻结产品候选 `6e9064319a51bedcd403d74d47dd45c067928013` 通过 Windows 远程桌面所呈现的本机右屏真实像素，
完成精确 `1,073,741,824` bytes、128/128 Segment 的恢复、WholeFileDigest、安全发布、final reopen 和
发送端/接收端外部 SHA-256/BLAKE3 配对。Decoder 未接收 source、摘要、Session 或 sender report；用户只在
Decoder 结束后回传四份 JSON。就**远程 1 GiB 功能恢复**而言，本次为 PASS。

原始 Gate 仍如实为 FAIL：`VerifiedEncodedBytesPerUniqueFrame = 8,626.510998634209`，低于现行
`16,384 B/unique frame` 硬门，Receiver Gate 因且仅因该后置性能检查 exit 1。用户在看到准确数值、原始退出码和
现行路线边界后，明确选择“**仅本次明确豁免**”。因此：

```text
G21 final status: PASS_WITH_SINGLE_RUN_USER_WAIVER
Remote 1 GiB functional recovery: PASS
Eventual recovery: PASS
Strict Pass-0 zero pressure: FAIL
16 KiB/unique raw hard-gate result: FAIL
32 KiB/unique engineering target: FAIL
G22: NOT STARTED
```

该豁免只作用于下述唯一 Run/Session，用于关闭 G21；它不改写原始报告，不把 exit 1 伪造成 exit 0，不修改指标分母、
协议、产品代码或 provider 分支，也不取消未来运行的 16 KiB/unique 硬门。后续引用本次 G21 结论时必须保留
`PASS_WITH_SINGLE_RUN_USER_WAIVER` 限定，不能缩写成“未豁免性能 PASS”。

## 2. 身份、顺序与实际像素边界

```text
Product source commit: 6e9064319a51bedcd403d74d47dd45c067928013
Gate/tool embedded commit: 959678340d1946fed4fec01b4410a250b533daa3
Sender RunId: 132e1a54d2134d2e828593dbeff210da
Receiver RunId: 2c4f578663c0b3762bb0396acb2021ab
SessionId: daba04b1c7c8c22a31604ea68dd61f8f
SessionTag: 15447616161310190557
Profile: PB-Unified-SC6-V3
VisualProfileId: 0x5042554E49534333
Layout: 10
```

Receiver 比 Sender 早启动 `46,343 ms`。Receiver 使用默认生产配置和 WGC，只捕获本机完整右侧物理屏幕
`\\.\DISPLAY2`；`\\.\DISPLAY1` 受保护，没有输入自动化。实际接受的 canonical canvas 始终为
`origin=(320,180)`、`scale=1.0x1.0`、marker residual `0 px`。远控软件由用户确认为 Windows 远程桌面；远控软件自身
的真实 FPS 未知，不能用远程显示器报告的 refresh rate 代替。payload 权威只来自 Receiver 捕获的右屏像素。

Sender 使用精确 1 GiB 的远程 OS CSPRNG source，参数包含 `--logical-fps 15 --manual-stop --loop` 且不含
`--seconds`；无自动 Sender deadline。Receiver 完成后，用户按 Q/Enter 正常停止 Sender，Encoder exit 0，source
post-stop 双摘要随后成功生成。

## 3. 恢复、吞吐与资源事实

| 项目 | 结果 |
| --- | ---: |
| Segment | 128 / 128 |
| verified raw / encoded bytes | 1,073,741,824 / 1,073,741,824 |
| Receiver elapsed | 9,102,167 ms |
| Receiver observations / unique | 170,244 / 124,470 |
| Receiver `UniqueVisualFPS` | 13.753410205098787 |
| Sender submitted frames / FPS | 143,283 / 14.985853399103439 |
| `VerifiedEncodedBytesPerUniqueFrame` | 8,626.510998634209 |
| accepted Transport blocks | 1,986,034 |
| `DeferredResourceBusy` / `OuterFecQuotaExceeded` | 970,220 / 970,220 |
| peak active Decoder | 8 / 8 |
| peak reserved Decoder bytes | 457,201,696 / 1,073,741,824 limit |
| Receiver process WS/private peak | 349,777,920 / 618,045,440 bytes |

配对 busy/quota 单调、相等，且只在有界 8-slot active-decoder window 到达历史峰值时出现；后续 Carousel 使用新的
repair IDs，最终释放容量并完成全部 Segment。`receiver-checks.json` 因而如实记录
`eventualRecoveryPassed=true`、`strictPass0ZeroPressurePassed=false`。所有真实 resource rejection、protocol limit、
FEC OOM、orphan drop/exhaustion/conflict、Outer conflict、lane FEC/CRC/identity failure 均为 0；current-run frame
coverage complete，counter overflow false。发布后无 `.part` 或 `.resume` 残留。

## 4. 独立摘要与进程终态

发送端在正常停止后独立读取 source；本机审计器重新完整读取已发布文件。两侧结果严格相等：

```text
bytes:  1,073,741,824
SHA256: e6ec3a7f5643f7b04ca5b90ce9510fb8388410b562b329fb70fe4cb837a0d323
BLAKE3: db460e2c8a260f885f3a8a1b0d4d47a5d04f9d74e648c8ce4a44626b03185063
```

Receiver `final.json` 为 `Completed`，WholeFileDigest、rename、final reopen、published 均为 true；原始
Receiver Gate exit 1，`failure.txt` 精确为：

```text
published file does not meet the G21 16 KiB/unique hard gate
```

Sender `encoder-report.json` 为 `Stopped`，source preparation/stability true，Encoder exit 0。跨机审计的全部功能检查
为 true；仅 `hard16KiBFrameMetricPassed` 与作为独立观察保留的 `strictPass0ZeroPressurePassed` 为 false。

## 5. 权威证据路径与哈希

### 5.1 Receiver 运行

```text
<repo>\build-unified-release\g21-remote-1gib-manual-08984cde1caf4130967bba5d6612b3b8\verified-local-receiver\runs\1gib6h-a79f9021c9814d8ca89f38fa41631878
```

已发布文件：

```text
receiver\recovered\g21-1gibmanual-132e1a54d2134d2e828593dbeff210da.bin
```

Receiver 独立审计：

```text
analysis\receiver-independent-audit.json
SHA256: b45b77eb3078d535abcd1552827ffd9bb152e37340d908f2c907c9ed383318c8
```

### 5.2 Sender 回传证据

```text
<repo>\artifacts\g21-remote-1gib-2026-09-07\successful-run-sender-evidence-132e1a54d2134d2e828593dbeff210da
```

| 文件 | bytes | SHA-256 |
| --- | ---: | --- |
| `source-manifest.json` | 2,529 | `d19dca14f089517e76dbeee02e61c79918e224b27a9b098464024a9622088ea8` |
| `source-poststop-digests.json` | 619 | `52579ec0178381bb191db46075f84b7adaee2c555a1df3a2f9b99cf43da1a907` |
| `encoder-report.json` | 2,545 | `b95613c44b241a4519acb3eb81984de908f365522da4c413fc97837f12fb4bbd` |
| `process-exit.json` | 80 | `0bc021cfc21bf394fe9945a4886452f6d31cc3bd5a4c7ed4eaf4c96bbe3431ba` |

### 5.3 跨机审计与用户豁免

```text
<repo>\artifacts\g21-remote-1gib-2026-09-07\remote-run-cross-host-audit-132e1a54d2134d2e828593dbeff210da.json
SHA256: 23d45347a42e923148ab3975ebef5277057d4895bd731a083209d8df24a423eb

<repo>\artifacts\g21-remote-1gib-2026-09-07\g21-single-run-waiver-132e1a54d2134d2e828593dbeff210da.json
SHA256: d45e9b4ecebc50fdee65e782835a7edb6aabe609c2581e463ea4e1fde01c78da
```

跨机审计原始结论保持：`functionalRemoteOneGiBPassed=true`、`hard16KiBFrameMetricPassed=false`、
`documentedG21GatePassed=false`。后生成的 create-only waiver 记录用户对该唯一运行的验收覆盖；二者不得相互覆盖。

### 5.4 运行包

```text
Remote Encoder v5 ZIP SHA256:
e2aa72914326fad9484244caf86cfdf5e7529cc20460ef29e1b113b152cda390

Local Receiver v5 ZIP SHA256:
354a53fb94398e58c70a4b33a59ba3ba84ff5c37940993e635ded59a29b8ab6b

Encoder EXE SHA256:
f63f7773b8de829da6536664e4467abbf69670b8dc4a56ea53452cc4d49caaf0

PBUnifiedRemoteGate EXE SHA256:
dc2e4d2565bacfbf279fd4270085b22a815bde45efeeb6a0e8a7983ab106cd9e
```

## 6. 复核步骤

1. 对第 5.2 节四个 sender JSON 重新计算 SHA-256，并确认 manifest、post-stop digest 与 Encoder report 的
   RunId、文件名、bytes、source digest、build identity 和正常 exit 0 一致。
2. 读取 Receiver `final.json`、`receiver-checks.json`、`process-exit.json` 和 `failure.txt`，确认 128/128、完整发布、
   eventual recovery true、原始性能失败和 exit 1 均未被改写。
3. 对 published `.bin` 重新流式计算 SHA-256/BLAKE3，确认与 sender post-stop digest 及双方 whole digest 相等。
4. 检查 Receiver 根没有 `.part/.resume`；检查 lane/resource/conflict/orphan/coverage/counter 字段满足第 3 节边界。
5. 读取 create-only cross-host audit，再读取后生成的 single-run waiver；不得把 waiver 当成原始测量，也不得把原始
   hard-gate FAIL 当成文件恢复失败。

## 7. 剩余边界

- G21 按用户单次明确豁免关闭；未来复验仍执行 16 KiB/unique 硬门。
- 独立 live false-accepted codeword oracle 未提供，继续记为 `null/unavailable`，不能改写为 0。
- 32 KiB/unique 工程目标未达到。
- 本次不证明特定 RDP FPS、网络带宽/延迟、其他 provider、20 GiB 现场能力或 G22 发布要求。
- G22 的包、SBOM、用户文档、LICENSE/仓库可见性与发布候选工作尚未开始。
