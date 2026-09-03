# Contributing to PixelBridge

PixelBridge 当前处于统一视觉 Profile 与大文件恢复重构阶段。贡献应围绕当前路线中的一个明确目标展开，并保持协议、资源和证据边界可审查。

## 1. 开始前

请依次阅读：

1. [`AGENTS.md`](AGENTS.md)：强制工程规则与 C++ 风格；
2. [`docs/UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](docs/UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md)：当前产品合同与 G00..G22；
3. [`docs/PixelBridge_最终技术路线与总体设计.md`](docs/PixelBridge_最终技术路线与总体设计.md)：总体架构与安全不变量；
4. [`docs/README.md`](docs/README.md)：模块/历史文档索引。

提交 Issue 或 Pull Request 前，请说明它对应哪个 Goal、修复哪个可复现问题，或为什么需要调整当前路线。旧 Phase 1.5、Direct/Shape/LF4 和 RemoteVisual Step 文档是历史实现/证据，不能覆盖 Unified 产品合同。

## 2. 贡献范围

优先接受：

- 明确 Goal 的实现或收口；
- wire/parser/resource/lifetime 的可复现缺陷修复；
- 能证明失败机制的边界、冲突、恢复和故障注入测试；
- CPU reference 与 D3D11 accepted-byte 语义一致性；
- 文档事实边界、可复现构建和依赖基线改进。

避免：

- 与当前 Goal 无关的大范围重命名或格式化；
- 新建一套与现有 PBProtocol/PBReceiver/PBStorage/runtime 平行的架构；
- 仅提高理论 bits/cell、但没有最终 verified output 的优化；
- 按远控软件品牌选择协议、阈值或解码分支；
- 隐藏 payload IPC、自动输入控制或依赖用户私有环境的路径；
- 为让测试通过而删除断言、降低阈值、跳过坏样本或重新分类错误。

## 3. 分支与提交

- 每个分支/提交只处理一个可解释目标；
- 提交前先运行 `git status --short --branch`，避开无关用户修改；
- 只用显式路径暂存，不使用 `git add .` 或 `git add -A`；
- 不提交 build tree、vcpkg installed tree、`.part`、resume、日志、Replay、MP4、大 fixture、安装包或凭据；
- 不 rewrite 已共享历史，不使用 force-push 覆盖他人提交；
- wire/Golden/profile/persistent schema 变化必须在同一提交中更新兼容说明和对应测试。

建议提交主题：

```text
feat(protocol): ...
feat(sender): ...
feat(receiver): ...
feat(modulation): ...
feat(demod): ...
fix(storage): ...
test(resume): ...
docs: ...
```

## 4. C++ 与模块边界

- C++20；变量 lower camel case，函数 PascalCase；左大括号单独一行；
- 优先 `i++`；初始化后不再改变的局部变量使用 `const`；
- 名称要表达完整语义，避免 `g`、`v` 等不透明缩写；
- 核心库不得依赖 Qt；Qt 只负责 UI/controller adapter；
- 所有 wire/persistence 必须显式 little-endian 序列化，不直接持久化 C++ struct；
- 所有 size/offset/count/add/multiply/range 使用 checked arithmetic；
- 输入、capture pixels、resume state、descriptor、文件名均视为不可信；
- 队列、cache、reassembly、active decoder 和 retry 必须有界；
- Whole-file digest 与安全发布完成前，不得把输出标记为完成。

## 5. 构建

Windows x64 默认开发构建示例：

```powershell
cmake -S . -B build-unified-release -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DPB_QT_ROOT=D:/Qt6.10.1/6.10.1/msvc2022_64 `
  -DBUILD_TESTING=ON `
  -DPB_BUILD_TESTS=ON `
  -DPB_BUILD_APPS=ON `
  -DPB_BUILD_TOOLS=ON

cmake --build build-unified-release --config Release --parallel
```

Qt/vcpkg 的实际位置可以不同，不得把个人绝对路径写进受管配置。更多 build 子图见 [`README.md`](README.md)。

## 6. 测试原则

正确性优先，但不对每个小改动执行全量回归。按路线的三个检查点组织测试：

1. 协议/流式/恢复；
2. 统一 Profile/GPU；
3. 最终产品。

普通 PR 只需：

- 构建受影响 target；
- 运行最相关的 test executable/CTest regex；
- parser/state machine 变化覆盖 malformed、boundary、conflict、resource case；
- `git diff --check`；
- 在 PR 说明中列出实际命令、结果和未运行门禁。

只有路线 G20 或 release freeze 才默认运行完整 Release CTest。真实显示、WGC/DXGI、ROI selector 和远程链路必须使用明确 opt-in Gate，不能放进普通无交互测试。

## 7. Graphics/capture 贡献

- 所有 ROI 坐标为 PMv2 physical pixels；
- WGC frame surface 的可用期受 frame-pool lease 限制；
- 先复制/crop 到 PB-owned texture，再归还 lease；
- capture/device/monitor/resize 变化创建新 epoch 并排空旧工作；
- D3D11 immediate context 默认单 owner thread；
- `ID3D11DeviceContext::Flush` 不是 GPU completion；
- fast path 不允许无报告的 raw ROI GPU→CPU→GPU 往返；
- CPU/GPU 比较 accepted Control/Transport、FEC/CRC/identity/erasure 语义，不要求浮点 soft metrics bitwise 相同。

真实桌面测试必须遵守路线的 DISPLAY2 约束，不自动移动鼠标、窗口或修改显示配置。

## 8. Pull Request 应包含

- 目标/问题与对应 Goal；
- 关键设计选择和不变量；
- 受影响公共接口、wire/Golden/persistence 兼容性；
- 可复现步骤或测试命令；
- 结果与关键 artifact digest；
- 未执行的 GPU、native、remote、long-file 或 ASan Gate；
- 性能变更的真实 numerator/denominator；
- 风险与回滚方式。

审查时优先关注：协议冲突、算术溢出、资源耗尽、partial/corrupt persistence、WGC/D3D lifetime、queue/backpressure、race/shutdown、色彩/range/pitch、duplicate/reordered frames 和 silent fallback。

## 9. 安全与敏感数据

不要在 Issue/PR/commit 中提交：

- token、API key、密码、私钥或签名材料；
- 真实用户文件、恢复输出或文件名；
- 远程连接凭据、私有主机地址或包含个人信息的截图；
- 未脱敏的运行日志/环境快照；
- 仓库外的系统凭据、SSH 或浏览器数据。

安全问题应先提供最小复现、受影响 commit/路径和失败机制；不要把真实敏感 payload 附在公开讨论中。

## 10. 事实表述

请精确区分：

- compile/build PASS；
- unit/reference PASS；
- WARP/GPU parity PASS；
- receiver-only Replay PASS；
- WholeFileDigest + safe publish PASS；
- DISPLAY2 native PASS；
- real remote pixels PASS；
- Certified/Release-ready。

后一项不能由前一项自动推出。未执行就是未执行；环境受限就是环境受限。
