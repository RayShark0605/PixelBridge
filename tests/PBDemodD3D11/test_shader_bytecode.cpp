#include "demod_shader_bytecode.h"
#include "demod_shader_source.h"
#include "remote_visual_low_fps_shader_source.h"
#include "unified_visual_shader_source.h"

#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <span>
#include <string>

TEST_CASE("Embedded demod bytecode exactly matches the former runtime compiler contract", "[demod][shader-bytecode]")
{
    namespace detail = pbdemodd3d11::detail;
    struct ShaderFixture
    {
        const char* source;
        std::size_t sourceBytes;
        const char* entryPoint;
        std::span<const unsigned char> bytecode;
    };
    const ShaderFixture shaders[]{
#define PB_DEMOD_SHADER(source, entryPoint, member) {detail::source, sizeof(detail::source) - 1, #entryPoint, detail::member##Bytecode},
#include "demod_shader_entries.inc"
#undef PB_DEMOD_SHADER
    };
    // Twelve product entries plus the two gray foreground-mode kernels.
    REQUIRE(std::size(shaders) == 14);
    for (const auto& entry : shaders)
    {
        INFO(entry.entryPoint);
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
        const HRESULT compile = D3DCompile(entry.source, entry.sourceBytes, "PB-Demod-D3D11", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.entryPoint, "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &shader, &diagnostics);
        const std::string compilerDiagnostics = diagnostics ?
            std::string(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()) : std::string{};
        INFO(compilerDiagnostics);
        REQUIRE(SUCCEEDED(compile));
        REQUIRE(shader);
        REQUIRE_FALSE(entry.bytecode.empty());
        REQUIRE(shader->GetBufferSize() == entry.bytecode.size());
        REQUIRE(std::memcmp(shader->GetBufferPointer(), entry.bytecode.data(), entry.bytecode.size()) == 0);
    }
}
