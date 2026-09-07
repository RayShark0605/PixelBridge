# G22 — 双端 GUI 重建与 Windows 发布候选

## 1. 当前状态与本轮授权

- 状态：**IN PROGRESS**；2026-09-07 已完成 Encoder 新 GUI 的初版接线与无显示验证，Decoder/包/实屏尚未完成。
- 起始提交：`b86702fafcde60b1666674bd5478649affa1c559`。
- G21 已以 `PASS_WITH_SINGLE_RUN_USER_WAIVER` 关闭，详见
  [最终远控结果](UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md)。本轮不重跑 G21 性能调优，
  不改写其原始性能失败、运行身份或单次豁免边界。
- 用户明确要求扩大原 G22 的打包范围：**丢弃旧的双端界面，重新交付最终 GUI**，不是给旧控件换标题。
- 仅由当前任务亲自执行；不创建子智能体、不委派其他任务。
- 只可在明确限制到右侧实验屏幕时进行必要实屏检查；不得干扰左屏、抢占用户焦点或自动操作鼠标键盘。

## 2. 已确认的新产品要求

### Encoder

- 主体仅保留单文件选择、1..60 Hz 刷新率、开始传输及必要状态。
- 默认 15 Hz；从开始准备到停止完成期间不能调整刷新率。
- 用户补充确认：**暂不考虑超过 500 GiB 的文件，保持现有资源上限**；包含 0-byte，
  不重新引入旧的 8 MiB/单 Segment UI 限制。
- 准备完成后在当前屏幕全屏呈现 Unified 数据流；无限 Carousel 广播，不按时间、轮次或对端完成自动结束。
- Esc 表示停止传输；源变更、硬件失效、资源或协议安全错误仍必须安全停止，不能将“永不停止”解释为忽略错误。
- 新增真正的高级选项 Tab，设置必须绑定实际 runtime，不能提供无效或绕过安全门的控件。

### Decoder

- 主体仅保留输出目录、指定屏幕的 ROI、开始接收及必要状态。
- 进度只用文本表达：百分比、恢复速度（KB/s）、剩余时间；详细协议/运行数据移到高级页。
- 没有有效画面、丢帧或暂时停滞时持续等待，保留已验证恢复状态；尽可能利用后续 Carousel 修复。
- 仍只从捕获到的 ROI 像素恢复，不读取 Encoder source、报告、Session 状态或任何隐藏 payload 旁路。
- WholeFileDigest、安全发布、最终重新打开复验全部成功后才能显示完成；不覆盖已有文件。
- 高级选项单独成 Tab，默认不要求用户手动调参。

### 全屏与 Esc 补充确认

- 用户已确认：以开始时 Encoder 主窗口所在屏幕为目标，沿用远控 1920×1080 画布
  1:1 居中加中性背景；Esc 只在 Encoder 持有焦点时生效，不注册全局键盘钩子。

### 待确认，不得自行落地

- 用户已确认 Decoder：先选择显示器，再选整屏或框选；框选只覆盖所选显示器，精确坐标在高级页；
  开始/停止使用同一个按钮，停止保留断点；完整验证并落盘后自动停止，页面保留 100%/已完成，不自动弹窗或打开目录。
- 项目自身 LICENSE、公开分发或签名选择不得擅定；本轮本地候选不等于已公开发布或已选择开源许可证。

## 3. 源码核对与复用边界

| 现场入口 | 当前行为 | G22 处理 |
| --- | --- | --- |
| `apps/PixelBridgeEncoder/encoder_gui.cpp` | G15 大量状态行、只读高级折叠区、运行中可调 FPS | 重做界面，改为主页面/高级 Tab，锁定运行期 FPS |
| `apps/PixelBridgeDecoder/decoder_gui.cpp` | G16 进度条与多行协议指标、旧全桌面选区入口 | 重做简洁界面，明确显示器/ROI 选择与纯文本三项进度 |
| `apps/common/local_desktop_runtime.*` | 实际 Unified 发送/捕获/解调/恢复状态机，已有单屏全屏发送模式 | 沿用生产收发路径，仅按新 UI 合同作必要适配 |
| `libs/PBRenderD3D` | 独立 D3D11 owner；既有全屏模式为 `WS_EX_NOACTIVATE` | 保持数据绘制与 Qt 分离；不能靠全局 Esc 监听干扰其他程序 |
| `libs/PBScreenRegion` | PMv2、物理像素、单显示器 ROI 验证 | 复用坐标与边界规则；不得把 Qt 逻辑像素直接交给捕获 |
| `apps/common/application_model.*` | 已验证字节、平滑恢复速度、可空 ETA | 使用真实 snapshot，不用发送 FPS、FEC 收包量或未验证 bytes 伪造完成 |
| `apps/common/encoder_session_store.*` / `decoder_resume_store.*` | 双端持久恢复与有界状态 | 不因重做 UI 丢弃协议恢复能力 |

当前真实 Profile 为 `PB-Unified-SC6-V3`、`VisualProfileId=0x5042554E49534333`、layout 10。
保留正式 wire、8 MiB Segment、FEC、Golden、捕获 lease/epoch、500 GiB policy 和文件发布不变量。
历史枚举名中的 `UnifiedLc4` 不表示正在使用旧 LC4 wire，不能只根据名称改 Profile。

## 4. 分段交付与验证预算

1. 记录现场、明确新合同与待决事项，保留旧版基线证据。
2. 重建 Encoder：简洁 Tab UI、真实高级设置、固定运行期 FPS、全屏开始/Esc 生命周期。
3. 重建 Decoder：显示器/ROI、真实高级设置、开始/停止和真实百分比/KB/s/ETA。
4. 对受影响 controller/model/GUI 和必要的选区边界做定向测试，保存 offscreen 图像供界面检查。
5. 生成独立 Windows 候选包、manifest、verifier、SBOM/notices、用户说明；从新解压包验证身份、GUI smoke。
6. 受保护右屏执行 G22 最小 1 MiB 实际像素恢复，独立读回最终文件并核对摘要；不重跑全量 CTest、
   20 GiB、64 MiB/500 MiB/1 GiB 阶梯或远控性能矩阵。
7. 显式路径暂存并创建独立提交；提交后重配/重建，区分代码身份、构建身份与实屏证据身份。

每个阶段的缺失门禁保留为未执行；不能以 GUI smoke 或旧 G21 的像素证据代替新版 GUI 完整收发。

## 5. 首次现场与无显示基线证据

证据根：`artifacts/g22-gui-baseline-20260907-154250/`（本地文件，不提交 artifact）。

- `baseline.json`：起始 commit/tree、Git 状态、保护文件哈希、G21 权威与屏幕/输入边界。
- `baseline-build.log` / `baseline-build-result.json`：Release 构建双端成功，exit 0。
- `baseline-gui-smoke.log` / `baseline-gui-smoke-result.json`：两个旧 GUI 的 offscreen smoke **2/2 PASS**，1.49 s。
- `baseline-binary-hashes.json`：此次基线双端 EXE 的 SHA-256。
- 后续 `--build-identity` 查明：基线 EXE 仍嵌入 `959678340d1946fed4fec01b4410a250b533daa3`，
  不是 checkout `b86702f`；此次基线仅执行了增量 build。两份 `PixelBridge*-build-identity.json` 保留实际身份，
  不把这轮旧二进制 smoke 算作 G22 新代码验证。Encoder 新代码已先显式重新配置再构建。
- Qt 部署提示 `VCINSTALLDIR` 未设置；当前构建通过，但这不能作为 VC runtime 已独立部署的证据，打包阶段须核对。

复验命令（无真实窗口/捕获）：

```powershell
cmake --build build-unified-release --config Release --target PixelBridgeEncoder PixelBridgeDecoder --parallel 4
ctest --test-dir build-unified-release -C Release -R '^PixelBridge(Encoder|Decoder)GuiSmoke$' --output-on-failure
```

保护文件 `docs/PHASE1_GATE_REPORT.md` 起始 SHA-256：
`076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。
该文件保持原有未跟踪状态，不修改、不暂存、不提交。

## 6. Encoder 新界面第一阶段

- 整体替换 `encoder_gui.cpp`：主体只显示文件、1..60 Hz（默认 15）、开始与必要状态；真正的两个 Tab。
- 高级页：实际 `sessionStateRoot` 设置、明确确认后删除当前 Session、create-only 导出诊断报告和只读运行详情。
  RAW/zstd、FEC、Profile 与资源合同保持固定，不提供无效调参选项。
- 当前主窗口的 Win32 monitor identity 绑定既有 `singleMonitorFullscreen` runtime；实际 1:1 中性背景组合仍在 Qt-free runtime 中。
- 源文件预扫描/传输/停止期间禁用文件、FPS 和缓存设置，controller 也拒绝活跃运行的 FPS 修改；保留核心 runtime 的历史诊断能力。
- Esc 使用 `Qt::WindowShortcut` 且关闭 auto-repeat，不注册全局快捷键或键盘钩子，数据窗口维持 no-activate。
- 新偏好使用独立 `g22/` keys，不继承旧 Profile、240 Hz 或窗口位置；文件名和状态 QLabel 强制 PlainText。
- 无显示 smoke 使用生产 controller/预扫描/persistence，替换的只有 OS monitor 选择与 presentation owner，
  不创建实际数据窗口。覆盖默认/越界偏好、Tabs、缓存生效、准备期锁定、程序化修改 disabled 控件不影响当次 FPS、
  局部 Esc 连接、停止保留、取消删除/确认删除和设置落盘。

证据根：`artifacts/g22-encoder-20260907/`；`configure.log`、`build-encoder.log`、`build-encoder-font.log`、
`encoder-smoke-01.log`、`encoder-smoke-02.log`。最后一轮 1/1 PASS，0.28 s（尚非实屏证据）。
`preview-01/` 保留 offscreen 未发现系统字体时的方框问题；`preview-02/` 通过显式加载本机 CJK 字体恢复可读预览。
字体文件不复制进包，不增加生产字体依赖。截图只渲染自己的 offscreen widget，不读取桌面像素。

尚未验证：真实全屏/Esc 用户操作、左/右屏 native containment、真实端到端、Decoder 新版、独立 package。
