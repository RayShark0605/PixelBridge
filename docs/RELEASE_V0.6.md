# PixelBridge v0.6 发布说明

日期：2026-09-14。应用版本：0.6.0。Windows x64 便携双端包。

## 本版内容

1. **Unified全屏适配**：完整canonical画布按实际屏幕物理像素铺满；不裁切，不在远端外围保留原1:1居中灰边。16:10等宽高比横纵独立缩放。1080p、2560×1440、2560×1600、3840×2160有本地像素与CPU/GPU恢复覆盖。没有改变Windows/Citrix分辨率设置。
2. **灰阶非本机接收**：修复真实Capture入口没有采用经同帧Bootstrap校验的profile采样准入，整合灰阶forward采样CPU/GPU实现；灰阶实验profile允许有证据覆盖的低尺度路径，SC6正式阈值保持不变。
3. **大文件调度和诊断**：包含本任务已实现的GrayFast流式调度、显式空间交织、可配置分段、独立发送状态根和计时/资源遥测。保留反例和实验开关，不把现场元数据当payload或ACK。
4. **预算约束的动态解码器名额**：显式`--budget-bound-decoders`允许超过8个，但必须先通过既有1GiB共享FEC预算、单实例/维度/会话/恢复限制。**不是无限资源，也不是整个进程RSS被1GiB覆盖。正式默认仍为8个。**
5. **恢复安全不变**：冲突拒绝、输入边界、摘要、最终安全发布、重开验证、源稳定性与不覆盖目标文件等门均保留。

## 本次实际进展及边界

同一100000000字节远端TestMedium前缀、完整全屏、30fps、6MiB目标段、相同封存Encoder/Decoder：

| 现场配置 | 完整本机接收耗时 | 有效帧率 | 活动解码器峰 |
|---|---:|---:|---:|
| 额外开启实验首轮65% | 13分2.615秒 | 7.49565 | 16 |
| 不开启首轮65%，使用100% | 8分45.968秒 | 7.50948 | 4 |

耗时下降32.79%；重复收到已完成分段的块数37277→4784；两次最终摘要、发布、重开及外部SHA均通过，资源/延期/冲突拒绝为0。**上述是版本变更前的Q/P2封存候选实测，不是0.6.0重新编译后的一次新实传。**

当前全屏仍慢于旧居中100MB的5分29.033秒；有效帧率约7.5，优化未结束。完整TestMedium/TestBig的最佳历史合计1小时11分52.469秒属于O旧居中候选，不归给本版。任意远控链路/缩放/4K实屏、大文件普遍最优均未认证。

## 使用

- 将整个ZIP解压到新目录；不要仅复制EXE，也不要覆盖旧包来混合DLL。
- Encoder位于`Encoder/PixelBridgeEncoder.exe`，Decoder位于`Decoder/PixelBridgeDecoder.exe`。
- GUI常规入口双击EXE。选中实际可見的传输区域，保持完整码面无遮挡。
- 正式profile和默认资源策略不变。研究中的GrayFast参数不能当作所有链路的通用最佳配置。
- 最近全屏100MB复现参数（**实验配置，不自动写入GUI默认**）：Encoder使用`--profile unified-gray-fast --logical-fps 30 --segment-target-mb 6 --grayfast-spatial-interleave`，**不传**`--grayfast-short-initial-airtime`；Decoder使用`--profile unified-gray-fast --budget-bound-decoders`并选择实际屏幕ROI。
- `.resume`应按原会话和受支持策略恢复；不要修改或强行复用未知/冲突状态。用实验动态名额写出的>8活动状态不能强塞给默认8名额模式。

## 发布身份与验证

版本及来源以包内`package-manifest.json`、运行`--version`/`--build-identity`为准，外部`.seal.json`绑定ZIP和manifest哈希。包内含独立验证器、SBOM及Qt/vcpkg/MSVC notices。哈希封印用于完整性，不等同于Authenticode签名。

项目自身许可证仍为既有`NOASSERTION`，本次没有代选新许可证、签名或公开上传。干净源码封装、已提交树与构建输入匹配、解压后完整性/版本/无窗口GUI smoke的实际结论见发布目录`DELIVERY_VERIFICATION.json`；本文不预先替代这些检查。因用户要求立即收尾，不追加新远控/大文件实验。

更完整的工程状态见`SESSION_HANDOFF_20260914_V0.6.md`；下一任务直接使用`NEXT_TASK_PROMPT_V0.6.md`。
