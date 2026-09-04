#include "demod_shader_source.h"
#include "remote_visual_low_fps_shader_source.h"
#include "unified_visual_shader_source.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{

struct ShaderSource
{
    const char* source;
    std::size_t sourceBytes;
    const char* entryPoint;
    const char* outputName;
};

std::string CompileShaders()
{
    namespace detail = pbdemodd3d11::detail;
    const ShaderSource shaders[]{
#define PB_DEMOD_SHADER(source, entryPoint, member) {detail::source, sizeof(detail::source) - 1, #entryPoint, #member "Bytecode"},
#include "demod_shader_entries.inc"
#undef PB_DEMOD_SHADER
    };
    std::ostringstream generated;
    generated << "// Generated at build time. Do not edit.\n#pragma once\n\nnamespace pbdemodd3d11::detail\n{\n";
    for (const auto& entry : shaders)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
        // Preserve the former runtime compiler contract for every configuration.
        const HRESULT compile = D3DCompile(entry.source, entry.sourceBytes, "PB-Demod-D3D11", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.entryPoint, "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &shader, &diagnostics);
        if (FAILED(compile))
        {
            std::cerr << entry.entryPoint << ": D3DCompile failed, HRESULT=0x" << std::hex << compile << '\n';
            if (diagnostics)
            {
                std::cerr.write(static_cast<const char*>(diagnostics->GetBufferPointer()),
                    static_cast<std::streamsize>(diagnostics->GetBufferSize()));
            }
            throw std::runtime_error("Shader generation failed; previous output was not replaced");
        }
        const auto* const bytecode = static_cast<const unsigned char*>(shader->GetBufferPointer());
        const std::size_t bytecodeBytes = shader->GetBufferSize();
        generated << "\ninline constexpr unsigned char " << entry.outputName << "[] =\n{\n";
        for (std::size_t index = 0; index < bytecodeBytes; index++)
        {
            if (index % 16 == 0)
            {
                generated << "    ";
            }
            generated << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(bytecode[index]) << ',';
            generated << (index % 16 == 15 || index + 1 == bytecodeBytes ? '\n' : ' ');
        }
        generated << "};\n";
        std::cout << entry.entryPoint << ": " << bytecodeBytes << " bytes\n";
    }
    generated << "\n} // namespace pbdemodd3d11::detail\n";
    return generated.str();
}

void WriteGeneratedHeader(const std::filesystem::path& outputPath, const std::string& generated)
{
    // CMake owns one writer per configuration. Never publish partially compiled output.
    std::filesystem::path temporaryPath = outputPath;
    temporaryPath += L".tmp";
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(temporaryPath, std::ios::binary | std::ios::trunc);
    output.write(generated.data(), static_cast<std::streamsize>(generated.size()));
    output.close();
    if (!MoveFileExW(temporaryPath.c_str(), outputPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        throw std::runtime_error("Cannot replace generated shader header: " + std::to_string(GetLastError()));
    }
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount != 2)
    {
        std::cerr << "Usage: PBDemodShaderCompiler <output-header>\n";
        return 2;
    }
    try
    {
        const std::string generated = CompileShaders();
        WriteGeneratedHeader(std::filesystem::path(arguments[1]), generated);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
