# third_party

生产依赖与测试依赖均由根目录 vcpkg manifest 管理，安装产物位于各 build tree
的 `vcpkg_installed/`，不入库。`vcpkg-ports/` 只保存需要固定尚未进入 builtin
registry 的 overlay port，不 vendoring 第三方源码。

当前 Wirehair overlay 固定官方 2.0.0 commit
`067ca7cdb66aed424ec23f97557429bf791c6f0c`，只产出静态库并关闭上游测试、内部
codec-v2 工具、benchmark、native tuning、双库构建和私有实验开关。精确 source
hash、license 与 wire-profile 文档入口见 [`WIREHAIR_BASELINE.md`](WIREHAIR_BASELINE.md)。
