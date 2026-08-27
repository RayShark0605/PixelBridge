# PBScreenCaptureWgc 验证记录

## 范围与基线

- 日期：2026-08-27。
- 开始时 HEAD：f779094（physical-pixel single-monitor screen region selection）；工作区干净。
- 范围：新增 WGC backend、其私有 lease/state/GPU 实现、测试、CMake 接入和文档。
- 不改现有 wire/API、FEC、Golden 字节、文件恢复规则、应用默认入口或显示设置。
- 复用 PBScreenRegion、PBRenderD3D 测试数据窗口、PBProtocol checked arithmetic 和现有
  编译/ASan/静态链接/no-Qt 门禁，不创建第二套区域选择器或 renderer。

环境：Windows 10.0.26200、Windows SDK 10.0.26100.0、Visual Studio 2022 x64、
MSVC /W4 /WX、CMake 3.31.6；D3D 测试使用 WARP/debug layer 和目标显示器硬件 adapter。
真实桌面是 HDR/PQ、10-bit output；native oracle 使用独立显示配置给出的 SDR white
而非捕获结果校准期望值。默认 BGRA 对 HDR 的拒绝未被绕过。

## 最终门禁

最终结果在提交前由对应日志确认。两棵 build tree 的常规测试可并行，但真实桌面
native gate 必须串行，避免两份 topmost fixture 互相遮挡；该拆分不是跳过测试。

| Gate | 结果 | 证据 |
|---|---|---|
| Release 完整 default-target build | 通过 | build-wgc-release/build-final.log |
| RelWithDebInfo + MSVC ASan 完整 default-target build | 通过 | build-wgc-asan/build-final.log |
| Release 全部非 native CTest | 77/77；260.83 s | build-wgc-release/ctest-final-nonnative.log |
| ASan 全部非 native CTest | 188/188；264.48 s | build-wgc-asan/ctest-final-nonnative.log |
| Release native WGC，连续 5 次新进程 | 5/5；10.13 s 合计 | build-wgc-release/ctest-native-final.log |
| ASan native WGC，连续 5 次新进程 | 5/5；14.94 s 合计 | build-wgc-asan/ctest-native-final.log |
| WGC owner/lease 单测，Release 与 ASan 各 20 次 | 已通过 | 两棵 build tree 的 ctest-repeat-unit.log |
| cppcheck 2.21.0，生产 5 个 .cpp | 0 diagnostics | build-wgc-release/cppcheck-final.log |
| 默认 WGC native gate OFF / 库仍在构建图中 | 通过 | build-wgc-release/default-off-verification.log |
| CMake capability/build-option 矩阵 | 16 组通过 | PBWgcBuildOptions |
| git diff --check / staged scope audit | 通过；仅 29 个任务文件 | build-wgc-release/staged-scope-audit.log |

完整测试集合合计为 Release 78/78、ASan 189/189；最后一个 native CTest 是单独串行
并重复运行的，不存在漏测。两种配置的 WGC 详细断言均为：

- owner/lease/state：17 cases / 560 assertions，额外各连续通过 20 次；
- WARP crop/fence/query：1 case / 371 assertions；
- optional-capability COM mocks：3 cases / 45 assertions；
- native capture：每次 2 cases / 143 assertions，额外各连续通过 5 次。

详细输出保留在两棵 build tree 的 ctest-wgc-assertions.log 和 ctest-native-final.log。
native 实测 cursorDisableAvailable/cursorExcluded 均为 true，MinUpdateInterval 可用且
读回匹配，unpackaged Borderless 未获授权；普通关闭及延后清理完成后的 live leases 均为 0。

Release 常规测试包含 Phase-0 Fast/Large/Resume、Golden、原有单元测试和独立静态库消费测试。
ASan 配置同时包含仓库已有的 112 个 fuzz/corpus CTest 条目；这是 MSVC ASan +
deterministic/structured mutation，不冒充 libFuzzer、UBSan 或 ThreadSanitizer。
ASan 下 Phase-0 Large 的不注册属于原有 CMake 策略，本任务未修改或弱化它。

## 回放命令

以下从 D:\MyProjects\PixelBridge 执行；构建树由 .gitignore 排除，不随源码提交。
配置命令完整版本见 PBScreenCaptureWgc.md；Release 配置显式启用 TESTS/APPS/TOOLS、
PHASE0_GATE/WGC_GATE，ASan 配置额外启用 FUZZERS，BENCHMARKS 关闭。

~~~powershell
cmake --build build-wgc-release --config Release --parallel 4
cmake --build build-wgc-asan --config RelWithDebInfo --parallel 4
ctest --test-dir build-wgc-release --build-config Release --output-on-failure --parallel 4 --exclude-regex '^PBWgcNativeTests$'
ctest --test-dir build-wgc-asan --build-config RelWithDebInfo --output-on-failure --parallel 4 --exclude-regex '^PBWgcNativeTests$'
ctest --test-dir build-wgc-release --build-config Release --output-on-failure --repeat until-fail:5 -V -R '^PBWgcNativeTests$'
ctest --test-dir build-wgc-asan --build-config RelWithDebInfo --output-on-failure --repeat until-fail:5 -V -R '^PBWgcNativeTests$'
ctest --test-dir build-wgc-release --build-config Release --output-on-failure --repeat until-fail:20 -R '^PBScreenCaptureWgcTests$'
ctest --test-dir build-wgc-asan --build-config RelWithDebInfo --output-on-failure --repeat until-fail:20 -R '^PBScreenCaptureWgcTests$'
cppcheck --language=c++ --std=c++20 --platform=win64 --enable=warning,style,performance,portability --error-exitcode=1 --suppress=missingIncludeSystem -Ilibs/PBScreenCaptureWgc/include -Ilibs/PBScreenCaptureWgc/src -Ilibs/PBScreenRegion/include -Ilibs/PBProtocol/include libs/PBScreenCaptureWgc/src
git diff --check
git diff --cached --check
~~~

## 复审与覆盖台账

| 要求/风险 | 实现与验证 |
|---|---|
| callback 轻量、无裸对象悬空 | 独立 CallbackGate；只 acquire/metadata/Push；没有 FEC、disk、D3D submission 或 CUDA sync |
| source 是 frame-pool lease | move-only FrameLease；source copy marker 完成前不能 Close；partial submission failure 同样 drain |
| ROI 后端异步使用 | copy 与 consumer 两阶段 marker；source 已 Close 不等于 ROI 可覆盖；专门测试 resize 等待 consumer marker |
| 不把 Flush 当完成 | fence GetCompletedValue 或 event-query GetData 的明确完成条件；失败不伪装成完成 |
| 有界队列与锁外 Close | drop oldest / take newest；锁外退休仍计费；阻塞旧 Close 并并发注入 100 帧，验证默认 high-water 不超过 6 |
| 不信任几何/尺寸 | signed 64-bit extent、checked multiply/add/narrow、全 pool/ring 预算、同 device/format/sample/mip/array；负 origin/odd offset/错误源 descriptor |
| ContentSize / 环境变化 | pause → drain → live size 核对 → recreate → epoch；拒绝缓存初始尺寸覆盖 frame/live geometry；epoch 溢出 fail closed |
| 初始化/异常/关闭 | 各 native 初始化阶段故障；Create 失败保留既有 output；EpochStarted/Submit/Close/Shutdown 错误及 HRESULT；重入 Stop |
| GPU 超时 | 保留 source/ring/consumer；预分配 Flush1/device event；异步移交后不再访问 owner 的 backend 状态；不会提前释放或 detached polling |
| device lost | GetDeviceRemovedReason 是释放未完成 source 的唯一 removal 依据；记录 lastDeviceLoss；有限次数重建；失败/额度用尽终止 |
| 能力而非 OS 版本推断 | 实际 COM QI mock 覆盖缺失/失败/忽略 setter；Borderless 未获许可不设置；cadence 读回不匹配不声称 applied |
| 可独立证明的像素输出 | WARP 双退休路径：逐通道 BGRA 算术 oracle；真实 WGC：三套图样 exact equality；第三套必须在新 epoch 捕获 |
| 构建与兼容性 | Windows-only 静态 target；公开头/链接闭包 no-Qt；无协议、Golden、第三方 revision 改动；默认不运行桌面 gate |

### 实际发现并修复的问题

1. 真实 HDR 桌面不能用 BGRA 测试期望冒充 SDR。修正测试为显式 FP16，并使用
   DISPLAYCONFIG_SDR_WHITE_LEVEL 的独立 oracle；没有增加像素容差。
2. WinRT projection 的进程级 factory cache 在短生命周期 MTA 反复退出后出现
   GraphicsCaptureSession::IsSupported 访问异常。改用局部、非缓存的
   try_get_activation_factory，并用 MTA usage cookie 覆盖延迟退休的整个生命周期。
3. 在现有 pool 上 Close session 再建 session，真实 Recreate 回归返回 E_UNEXPECTED。
   普通 Recreate 保持 session 存活，只撤销 admission/handlers；真正 shutdown 才 Close。
4. SetWindowPos 成功不等于测试窗口已实际可见。增加 WindowFromPoint 的有界确认，
   不以放宽像素断言掩盖窗口遮挡。
5. 最终复审补强锁外 stale leases 的容量预留、consumer marker 的 resize 等待、
   初始 item size 滞后、Close 失败先于 consumer delivery 的 fail-closed 行为及
   startup owner-id 的发布顺序；已增加确定性回归。

## 仍未覆盖、也未作保证的部分

- 未改变用户显示设置；真实 OS 分辨率/方向/刷新模式切换未测试。尺寸变化由生产
  状态机模型、live-size resolver 和真实 D3D ring 的不同尺寸重建覆盖，native pool
  Recreate 在当前物理尺寸上验证。
- 未制造真实 TDR/device removal；恢复额度、未完成 source 的保留与错误传播有模型测试，
  真正的 Flush1 延迟清理已在硬件 WGC 路径执行。
- 未覆盖 portrait/hybrid GPU、跨 monitor migration、多 adapter、Windows 10 旧能力矩阵、
  MSIX Borderless 交互授权或 cursor 正好覆盖 ROI 时的逐像素排除认证。
- 输出是 capture backend 的 owned ROI 和元数据；未接入完整 Capture Normalize、
  Pilot/demod、CUDA、FEC、Decoder 文件恢复；不作 UniqueVisualFPS/VerifiedEncodedGoodput
  或 LocalDesktop certification 声明。
- 长时间不返回的 driver/WinRT/不守契约的 consumer 无法被本层超时抢占；OS/GPU 永远
  不完成且不报告 removal 时，保留固定大小的待退休资源，不谎报 shutdownComplete。
- cppcheck 不展开 Windows SDK/WinRT 系统头；其结果不能替代实际 MSVC、ASan 和 D3D
  debug-layer/native 运行证据。
