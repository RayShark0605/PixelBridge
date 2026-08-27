# PBScreenCaptureWgc：有界 WGC lease 与异步 ROI 退休

## 范围与依赖

本模块实现总体设计 §1.11、§21、§22.1–22.6、§23 的 WGC capture 基础设施。
复用 PBScreenRegion 的 ScreenCaptureRegion 和 PBProtocol 的 checked arithmetic；
独立静态 target PB::PBScreenCaptureWgc，不依赖 Qt，不新增第二套区域选择器、
渲染器或空壳 Capture Normalize 框架。不修改协议、Golden Vectors、FEC、文件恢复规则。

构建使用 Windows SDK 10.0.26100.0 或更新版本的 C++/WinRT headers。
运行时按实际 interface capability 探测，不以 SDK 版本冒充 OS 支持：
CreateForMonitor / CreateFreeThreaded 为必需能力；cursor、Borderless、
MinUpdateInterval 为可选能力。硬件设备显式建立在目标 monitor 的 DXGI adapter 上，
要求 feature level 11.0+、BGRA support，不使用 SINGLETHREADED，也不静默切到 WARP。
WARP 仅用于独立 D3D correctness test。

默认 BGRA8 路径限 SDR；HDR/linear advanced-color 输出必须显式选择
DXGI_FORMAT_R16G16B16A16_FLOAT，全链路保留浮点格式，不隐式 tone-map 或 CPU readback。
此处提供原始 WGC ROI 和 format/HDR/rotation/physical region/adapter/timestamp/epoch
元数据，不把它冒充已经完成信号归一化、Pilot 校准或颜色认证的数据。

## 线程和资源所有权

~~~text
WGC internal worker
  TryGetNextFrame
  ContentSize / SystemRelativeTime
  move-only FrameLease
  bounded FrameInbox (drop oldest; no Close under queue lock)
               |
               v
Dedicated MTA / D3D submission owner
  validate source texture device / dimensions / format / sample / mip / array
  CopySubresourceRegion -> PixelBridge-owned ROI slot
  End(event query), optional Signal(fence), Flush
               |
       copy completion proven
               v
  Close source Direct3D11CaptureFrame
  RoiConsumer::Submit(metadata, owned ROI, immediate context)
  completion marker AFTER consumer submissions, even on consumer failure
               |
       consumer GPU work completed
               v
  reuse ROI slot
~~~

- FrameArrived 不访问 Surface，不提交 D3D copy，不做 FEC、disk、CUDA sync 或用户回调。
  本层热路径没有每帧 heap wrapper allocation；WinRT/driver 内部开销不由此声明覆盖。
- callback 使用独立 shared gate，不捕获裸 capture/backend 指针。gate 禁用与 callback
  进入串行化；同时最多一个 callback 获取 lease。旧 epoch event 不能污染新 epoch。
- queue 默认最多 2 个 frame；ROI ring 默认 3 个 texture。full queue 先丢旧帧；
  owner 取最新帧并丢弃未提交的更旧帧。已提交 copy 的 source 绝不作为 stale 提前释放。
- 锁外正在关闭的 stale queue leases 继续占用 admission 容量；不能在旧批次关闭前
  把队列再次补满而产生瞬时额外 source leases。独立阻塞 Close 的并发测试验证这个边界。
- pool buffer count = queue limit + ROI slot count + 1；默认 source lease high-water
  上界为 6（包含 callback 正在获取的那一个）。队列上限 4，ROI slots 上限 8。
- maximumRoiBytes 约束整个 ROI ring；maximumCaptureBytes 约束
  全部 WGC pool surfaces 加 ROI ring 的显式像素存储。checked multiply/add/narrow
  在大纹理分配前执行。驱动、OS capture service 的额外内部存储不包含在这个逻辑预算中。
- Recreate 先退休并释放旧 ring，再建立新 ring，避免同时保留两套 owned ring 而超出预算。
- fence 支持时优先使用 fence；每 slot 仍预建 event query，以保证 Signal 在 copy
  已提交后失败时仍有退休证明。Flush 只是提交，不是完成证明。

## 消费者接口

公共头文件：libs/PBScreenCaptureWgc/include/pbscreencapturewgc/wgc_capture.h。

1. host 在 manifest 中声明 Per-Monitor V2；通过 PBScreenRegion 获取单 monitor 物理 ROI。
2. 提供 shared RoiConsumer，调用 WgcCapture::Create。失败不修改 output。
3. EpochStarted 在 owner 上通知新 epoch、设备和 capture environment；消费者必须
   丢弃旧 geometry/calibration/device-specific state。这里禁止 GPU submission。
4. Submit 只在同一个 owner 上借用 owned ROI 和 immediate context，并可提交短小的
   D3D11 工作。不得在返回后保存 texture/context/bindings 供未来访问，不得由其他线程、
   其他 context 或未接入退休协议的 CUDA stream 异步使用该 slot。
5. 允许从消费者调用 GetSnapshot / RequestStop。Stop 不得在 owner 上 join 自身，
   会请求停止并返回 WrongThread；真正 join 由 host 执行。
6. host 不能让对象析构与其他成员调用并发，也不能在消费者回调里销毁 capture 对象。

生产模块不包含 CPU Map/readback。逐像素测试的 staging texture 和 Map 位于 test oracle，
并不在 FrameArrived 或生产 backend 中。

## Epoch、重建与关闭

ContentSize mismatch：

重建尺寸使用实际 frame 的 ContentSize，并与重新解析的物理 monitor 几何核对；
只有环境变化触发的请求使用新的 live monitor extent。不把初始 GraphicsCaptureItem.Size
当作必然已经更新的尺寸，也不在尺寸冲突时默默用旧 buffer 继续捕获。

~~~text
stop admission -> drop queued frames -> revoke event handlers
  -> wait for active acquisition and all submitted copy/consumer work
  -> FramePool.Recreate on the existing live session
  -> rebuild owned ROI ring
  -> checked CaptureEpoch++
  -> EpochStarted (reset consumer state)
  -> attach a new callback gate -> resume admission
~~~

普通 frame-pool Recreate 保留 capture session，不能先 Close 再在同一 pool 上创建
session：原生回归曾证实后者返回 E_UNEXPECTED。Pause 在这里表示停收和撤销 handler，
而非终止 session。pool 里的未获取旧帧由 Recreate 丢弃。

monitor geometry、DPI、rotation、refresh、output color space/bit depth 或 DXGI factory
变化也触发重建。monitor 不可用、ROI 不再位于目标 monitor、unsupported color mode 等
情况 fail closed；不夹取 ROI，不偷偷切换 monitor。环境轮询为 200 ms，ContentSize
事件不等待该轮询。无法提供事件之间发生又恢复的所有显示模式变化历史。

只有 GetDeviceRemovedReason 的权威失败才允许在没有 copy completion 的情况下退休旧
GPU source。每个实例的自动 device recovery 次数有限（默认 1，最多 8），重建前 drain，
重建成功后开启新 epoch；恢复失败即结束，不无限重试。snapshot 同时记录 lastDeviceLoss
和当前 error。CaptureEpoch 不允许 wrap。

关闭次序为：停收、撤销事件、等待 callback、退休 GPU 工作、Close frames、
Close session/pool、释放 owned graphics resources、注销 device-removed event、
释放 OS wait handles、释放 MTA usage。所有正常/部分初始化/异常分支复用这条路径。

### GPU 超时不等于允许归还 lease

正常工作和 drain 都有 GPU deadline。超时会停止输入；Stop join 的 owner 不进行无限
GPU 轮询。预先分配的 Flush1 completion event / device-removed event 接管固定大小的
资源集合，直到 source 不再被 GPU 使用，再执行延后 cleanup。若此时仍有停收前进入的
acquisition callback，则将同一个 OS wait 转到其 idle event；不阻塞 thread-pool worker。

- snapshot.deferredCleanup = true 且 shutdownComplete = false 时，绝不宣称关闭成功。
- 即使 host 此时销毁 WgcCapture，退休 owner 仍保留所需 source leases、ROI ring 和 consumer。
- CoIncrementMTAUsage 保证延后退休期间 MTA 不随原 owner 线程退出而消失。
- 若 OS/GPU 永不完成、也不报告 removal，或 OS cleanup apartment/wait 失败，保留该实例的
  有界资源，不强行 Close、不重试创建新 capture、不产生 detached polling thread。
- deadline 不能抢占 driver/WinRT 内部阻塞，也不能抢占违反“短小提交”契约的消费者。

激活工厂采用非缓存 try_get_activation_factory。不得改回会使用 process-wide factory
cache 的 static projection/get_activation_factory：原生多次创建/销毁、宿主未初始化 COM
的回归曾发现短生命周期 MTA 之间缓存指针失效。模块不清空宿主的全局 factory cache。

## 可选能力的真实语义

| 能力 | 行为 | 不保证的事情 |
|---|---|---|
| Cursor | QI session2；默认 set false，再读回；失败保留错误与 cursorExcluded=false | API 不存在时不伪装成已排除 cursor |
| Borderless | QI session3；显式请求且有 package identity 才发起 RequestAccessAsync；owner 非阻塞轮询；Allowed 后才 set false | setter 成功不保证其他 session 不要求边框 |
| MinUpdateInterval | QI session5；请求存在时设置、读回；缺失/失败不阻断 capture | setting/readback 不是 CaptureFPS 或 UniqueVisualFPS |

unpackaged Win32 保留 capture border；ROI 不得依赖屏幕最外缘像素。此模块不请求默认
borderless 权限，不因优化失败而变更协议或输出格式。

## 可复现验证

~~~powershell
Set-Location D:\MyProjects\PixelBridge
cmake -S . -B build-wgc-release -G "Visual Studio 17 2022" -A x64 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0" "-DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DPB_BUILD_TESTS=ON -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_WGC_GATE=ON
cmake --build build-wgc-release --config Release --parallel 4
ctest --test-dir build-wgc-release --build-config Release --output-on-failure --parallel 4
cmake -S . -B build-wgc-asan -G "Visual Studio 17 2022" -A x64 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0" "-DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DPB_BUILD_TESTS=ON -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_WGC_GATE=ON -DPB_BUILD_FUZZERS=ON
cmake --build build-wgc-asan --config RelWithDebInfo --parallel 4
ctest --test-dir build-wgc-asan --build-config RelWithDebInfo -L wgc --output-on-failure
~~~

Native gate 为显式 opt-in，临时显示自己的小型测试窗口，不改显示模式、HDR 开关或 cursor
位置，关闭时销毁该窗口。不能同时运行两份使用同一桌面的 native gate。必须等 WindowFromPoint
确认测试窗口可见，不能把窗口被遮挡时捕获的桌面颜色当成 crop/色彩错误。

- PBScreenCaptureWgcTests：生产状态机的 lease、queue、尺寸/epoch、延迟退休、
  partial submission、重入 Stop、并发 producer、超时保留、device recovery 和 failure cleanup。
- PBWgcCapabilitiesTests：真实 C++/WinRT COM mock 的缺失 interface、失败/忽略 setter、
  denied/granted Borderless、未请求 cadence，不按 OS 版本硬编码。
- PBWgcD3dTests：真实 WARP + debug layer；fence/query、独立逐像素 BGRA oracle、
  负 monitor origin、odd ROI offset、RowPitch、错误 source descriptor 和不同尺寸重建。
- PBWgcNativeTests：真实硬件 CreateForMonitor/CreateFreeThreaded；三组屏幕图样的逐通道
  exact equality、fence/query、实际 FramePool.Recreate、新 epoch 像素、部分初始化故障、
  真正的 OS-event deferred cleanup。
- 第三组图样只在 Recreate 后呈现，必须逐通道匹配且属于新 epoch；仅旧图样或 epoch
  数值变更不算通过。
- HDR native oracle 的 scRGB SDR white 来自独立 DISPLAYCONFIG_SDR_WHITE_LEVEL；
  按 1000 对应 80 nits 的定义转换为 FP16，RGBA 通道顺序显式处理，无误差阈值或自动校准。
- PBWgcRejectsNonPmv2Host、PBWgcBuildOptions，以及静态库独立链接/no-Qt/Golden/Phase-0 gates。

这些结果不是 LocalDesktop certification。未覆盖的矩阵包括真正的显示模式切换、
portrait/hybrid GPU、多 monitor migration、真实 TDR/removal、Windows 10 下的可选能力缺失、
MSIX Borderless 授权流程和完整 demod/文件恢复链路；也不提供性能/unique FPS 承诺。

## API 依据

本次构建、测试、真实硬件证据和复审台账见
[PBScreenCaptureWgc_validation.md](PBScreenCaptureWgc_validation.md)。

- [CreateForMonitor](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor)
- [CreateFreeThreaded](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)
- [D3D11 GetData completion](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)
- [Flush1 completion event](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext3-flush1)
- [Device-removed event](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device4-registerdeviceremovedevent)
- [Borderless consent](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired)
- [SDR white level](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level)
