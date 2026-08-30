# PixelBridge RemoteVisual Channel Manifest v1

状态：`PixelBridge.RemoteVisualChannelManifest.1` 是 provider-generic 离线信道变换的实验记录格式。它不是 PixelBridge wire、Session descriptor、Receiver acceptance 输入或 Certified Profile 合约。

## 1. 责任边界

`PBRemoteVisualSimulator` 接收 production Encoder 生成的 BGRA8 raster，以及可选的同尺寸前一 raster。它只执行有界、确定性的 channel transforms：

- `resample`：显式输出尺寸、连续 scale/origin、`area` 或 `bilinear` filter 和 BGRA border；
- `block-replacement`：把当前 pipeline 阶段中指定矩形替换为 reference raster 的同位置像素，用于模拟远控块更新/陈旧区域。

模块不实现 Bootstrap、Demod、FEC、Transport、Outer FEC 或 Receiver。变换后的 raster 必须重新进入现有 production CPU/reference decoder；manifest 中的 hash 或 truth 参数不能改变 acceptance。

## 2. 规范化输入与 hash

输入可包含 row padding，但 canonical image 只包含 active BGRA8 像素。image BLAKE3-256 的输入严格为：

```text
UTF-8/ASCII "PixelBridge.RemoteVisualChannelImage.1" including its trailing NUL
+ width  u32 little-endian
+ height u32 little-endian
+ height rows of exactly width*4 active BGRA bytes
```

因此，相同 active pixels、尺寸和格式在不同 row pitch/padding 下具有相同 hash；不同二维尺寸不会仅因字节拼接相同而碰巧具有同一 domain input。manifest 自身的 BLAKE3-256 是 canonical UTF-8 JSON 的精确字节 hash，不含 BOM 或尾随换行。

## 3. Canonical JSON

字段顺序、标点和无空白编码由 `PBRemoteVisualSimulator` 固定生成。顶层字段依次为：

1. `schema`：固定 `PixelBridge.RemoteVisualChannelManifest.1`；
2. `version`：固定整数 `1`；
3. `seedHex`：16 个 lowercase hex 字符；当前确定性 transforms 不消费 seed，后续随机变换必须只从此 seed 派生；
4. `source`：`format=bgra8`、width、height、canonical image BLAKE3；
5. `referenceBlake3`：reference canonical image BLAKE3 或 `null`；reference 必须与 source 同尺寸；
6. `transforms`：按实际执行顺序记录；
7. `output`：最终尺寸、格式和 canonical image BLAKE3。

每个 transform record 包含 index、kind、输入尺寸/hash、reference 输入 hash、全部参数、输出尺寸/hash。所有 BLAKE3 均为 64 个 lowercase hex 字符。

为避免不同标准库的十进制浮点格式造成 manifest 漂移，scale/origin 使用 IEEE-754 binary64 原始位模式的 16 个 lowercase hex 字符：

- `scaleXBinary64`；
- `scaleYBinary64`；
- `originXBinary64`；
- `originYBinary64`。

`-0.0` 在序列化前规范化为 `+0.0`。NaN、Infinity、非正 scale、超出 policy 的 scale/origin 在任何 output publish 前拒绝。

## 4. 资源与失败语义

### 4.1 v1 resample 数学

source 的逻辑 pixel-edge `(u,v)` 映射到 output edge：

```text
x = originX + scaleX * u
y = originY + scaleY * v
```

`bilinear` 将 output pixel center `(x+0.5,y+0.5)` 反投影到 source center coordinates；反投影 center 位于 source extent 外时整像素使用 `borderBgra`，位于 extent 内时四个邻点按 edge-clamp 采样。`area` 将 output pixel 的完整 edge footprint 反投影，按与每个 source pixel 的交叠面积加权，footprint 落在 source extent 外的面积使用 `borderBgra`。四个 BGRA channel 独立计算，结果 clamp 到 `[0,255]` 后按非负值 half-up 取整。

零 transform 是 canonical identity：逐行复制 active bytes、去除输入 padding。精确 `scale=1/origin=0/output dimensions=source dimensions` 也走 bit-exact fast path。若提供 reference，所有 resample 以相同参数同步应用到 current/reference；`block-replacement` 在它出现的 pipeline 阶段复制 reference 同位置矩形，因此 transform 顺序具有语义并进入 manifest。

### 4.2 Resource policy

默认 policy 同时也是 v1 hard upper bound；调用者只能收紧，不能通过自定义 policy 扩大：

| 项目 | 上限 |
| --- | ---: |
| dimension | 8192 |
| pixels/image | 8 MiPixels |
| resident transformed bytes | 128 MiB |
| work units/plan | 256 Mi |
| transforms/plan | 64 |
| scale per axis | 0.125..4.0 |

所有 width×height、row extent、pixel bytes、resident peak 和 work accumulation 都先做 checked arithmetic。非法 view、reference mismatch、越界 rectangle、oversize、work excess 或 allocation failure 返回结构化错误；失败结果不包含部分 output 或部分 manifest。

这些 limit 是 simulator resource policy，不进入 frozen wire，不因 remote provider/version 改变。

## 5. 可重放规则

一次可审计实验必须保存：

- production input raster identity/hash；
- reference raster identity/hash（若使用）；
- canonical manifest JSON 及其 BLAKE3；
- transform 后 raster hash；
- production decoder 的 Bootstrap、geometry、metric、FEC、Transport 和 Receiver disposition。

相同 input/reference active pixels、plan、seed 和实现 revision 必须产生 byte-identical output、canonical manifest 和 digest。Simulator PASS 只证明离线信道模型，不等于 RemoteVisual field PASS 或 Certified profile。
