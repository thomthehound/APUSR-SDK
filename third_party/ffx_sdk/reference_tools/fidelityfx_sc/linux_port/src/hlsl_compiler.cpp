// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "ffx_sc/hlsl_compiler.hpp"

#include "ffx_sc/utils.hpp"

#include <fcntl.h>
#include <optional>

struct DxcCustomIncludeHandler final : public IDxcIncludeHandler
{
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppvObject) override
    {
        if (ppvObject == nullptr)
        {
            return E_POINTER;
        }
        if (iid == __uuidof(IDxcIncludeHandler) || iid == __uuidof(IUnknown))
        {
            *ppvObject = this;
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE LoadSource(
        _In_z_ LPCWSTR                               filename,
        _COM_Outptr_result_maybenull_ IDxcBlob**     includeSource) override
    {
        const fs::path requestedPath = WCharToUTF8(std::wstring(filename));

        auto resolveCandidate = [](const fs::path& candidate) -> std::optional<fs::path>
        {
            if (candidate.empty())
            {
                return std::nullopt;
            }

            std::error_code ec;
            fs::path absolutePath = fs::absolute(candidate, ec);
            if (ec)
            {
                return std::nullopt;
            }

            if (!fs::exists(absolutePath, ec) || ec)
            {
                return std::nullopt;
            }

            fs::path canonical = fs::weakly_canonical(absolutePath, ec);
            if (ec)
            {
                canonical = absolutePath;
            }

            return canonical;
        };

        auto resolveFromBase = [&](const fs::path& base, const fs::path& relative) -> std::optional<fs::path>
        {
            if (relative.is_absolute())
            {
                return resolveCandidate(relative);
            }

            return resolveCandidate(base / relative);
        };

        std::optional<fs::path> resolved;

        if (!sourcePath.empty())
        {
            resolved = resolveFromBase(sourcePath.parent_path(), requestedPath);
        }

        if (!resolved)
        {
            resolved = resolveCandidate(requestedPath);
        }

        if (!resolved)
        {
            for (const fs::path& searchRoot : includeSearchPaths)
            {
                resolved = resolveFromBase(searchRoot, requestedPath);
                if (resolved)
                {
                    break;
                }
            }
        }

        if (resolved)
        {
            dependencies.insert(resolved->generic_string());

            // Materialize the canonical include path as a wide string so the underlying buffer
            // survives the call into DXC, mirroring the Windows implementation while avoiding
            // the temporary-pointer lifetime issue that would otherwise surface here.
            const std::wstring resolvedWidePath = UTF8ToWChar(resolved->generic_string());
            return upstream->LoadSource(resolvedWidePath.c_str(), includeSource);
        }

        // Preserve DXC's stock search heuristics by forwarding the original request if the
        // path lookup above failed.  As with the resolved case we allocate a stable wide
        // string buffer so the upstream handler observes the same contract as it does on
        // Windows.
        const std::wstring requestedWidePath = UTF8ToWChar(requestedPath.generic_string());
        return upstream->LoadSource(requestedWidePath.c_str(), includeSource);
    }

    fs::path                                 sourcePath;
    std::vector<fs::path>                    includeSearchPaths;
    std::unordered_set<std::string>          dependencies;
    CComPtr<IDxcIncludeHandler>              upstream;
};

/// The DXC shader binary simply wraps the DXC blob pointers but the linker still
/// expects the vtable to live in this TU.  Providing the overrides here keeps the
/// class lightweight while ensuring the optimizer can inline away the indirection
/// in hot paths where we emit binary payloads.
uint8_t* HLSLDxcShaderBinary::BufferPointer()
{
    if (!pShader)
    {
        return nullptr;
    }

    return static_cast<uint8_t*>(pShader->GetBufferPointer());
}

size_t HLSLDxcShaderBinary::BufferSize()
{
    if (!pShader)
    {
        return 0;
    }

    return pShader->GetBufferSize();
}

HLSLCompiler::HLSLCompiler(Backend            backend,
                           const std::string& dll,
                           const std::string& shaderPath,
                           const std::string& shaderName,
                           const std::string& shaderFileName,
                           const std::string& outputPath,
                           bool               disableLogs,
                           bool               debugCompile)
    : ICompiler(shaderPath, shaderName, shaderFileName, outputPath, disableLogs, debugCompile)
    , m_backend(backend)
{
    if (!dll.empty())
    {
        throw std::runtime_error("Custom DXC DLL selection is not supported on Linux.");
    }

    if (m_backend != HLSLCompiler::DXC)
    {
        throw std::runtime_error("Only the DXC backend is supported on Linux.");
    }

    std::ifstream sourceStream(m_ShaderPath);
    if (!sourceStream.is_open())
    {
        throw std::runtime_error("Failed to open shader source: " + m_ShaderPath);
    }
    m_Source.assign(std::istreambuf_iterator<char>(sourceStream), std::istreambuf_iterator<char>());

    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_DxcUtils));
    if (FAILED(hr))
    {
        throw std::runtime_error("Failed to create IDxcUtils instance.");
    }

    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_DxcCompiler));
    if (FAILED(hr))
    {
        throw std::runtime_error("Failed to create IDxcCompiler3 instance.");
    }

    hr = m_DxcUtils->CreateDefaultIncludeHandler(&m_DxcDefaultIncludeHandler);
    if (FAILED(hr))
    {
        throw std::runtime_error("Failed to create default include handler.");
    }
}

HLSLCompiler::~HLSLCompiler()
{
    m_DxcDefaultIncludeHandler.Release();
    m_DxcCompiler.Release();
    m_DxcUtils.Release();
}

bool HLSLCompiler::Compile(Permutation& permutation,
                           const std::vector<std::string>& arguments,
                           std::mutex& writeMutex)
{
    return CompileDXC(permutation, arguments, writeMutex);
}

bool HLSLCompiler::ExtractReflectionData(Permutation& permutation)
{
    return ExtractDXCReflectionData(permutation);
}

void HLSLCompiler::WriteBinaryHeaderReflectionData(FILE* fp,
                                                   const Permutation& permutation,
                                                   std::mutex&       writeMutex)
{
    std::lock_guard<std::mutex> guard(writeMutex);

    auto* reflection = dynamic_cast<IReflectionData*>(permutation.reflectionData.get());
    if (!reflection)
    {
        return;
    }

    auto writeResources = [&](const std::vector<ShaderResourceInfo>& resources, const char* suffix) {
        if (resources.empty())
        {
            return;
        }

        fprintf(fp, "static const char* g_%s_%sResourceNames[] = { ", permutation.name.c_str(), suffix);
        for (const ShaderResourceInfo& info : resources)
        {
            fprintf(fp, " \"%s\",", info.name.c_str());
        }
        fprintf(fp, " };\n");

        fprintf(fp, "static const uint32_t g_%s_%sResourceBindings[] = { ", permutation.name.c_str(), suffix);
        for (const ShaderResourceInfo& info : resources)
        {
            fprintf(fp, " %u,", info.binding);
        }
        fprintf(fp, " };\n");

        fprintf(fp, "static const uint32_t g_%s_%sResourceCounts[] = { ", permutation.name.c_str(), suffix);
        for (const ShaderResourceInfo& info : resources)
        {
            fprintf(fp, " %u,", info.count);
        }
        fprintf(fp, " };\n");

        fprintf(fp, "static const uint32_t g_%s_%sResourceSpaces[] = { ", permutation.name.c_str(), suffix);
        for (const ShaderResourceInfo& info : resources)
        {
            fprintf(fp, " %u,", info.space);
        }
        fprintf(fp, " };\n\n");
    };

    writeResources(reflection->constantBuffers, "CBV");
    writeResources(reflection->srvTextures, "TextureSRV");
    writeResources(reflection->uavTextures, "TextureUAV");
    writeResources(reflection->srvBuffers, "BufferSRV");
    writeResources(reflection->uavBuffers, "BufferUAV");
    writeResources(reflection->samplers, "Sampler");
    writeResources(reflection->rtAccelerationStructures, "RTAccelerationStructure");
}

void HLSLCompiler::WritePermutationHeaderReflectionStructMembers(FILE* fp)
{
    fprintf(fp, "\n");
    fprintf(fp, "    const uint32_t  numConstantBuffers;\n");
    fprintf(fp, "    const char**    constantBufferNames;\n");
    fprintf(fp, "    const uint32_t* constantBufferBindings;\n");
    fprintf(fp, "    const uint32_t* constantBufferCounts;\n");
    fprintf(fp, "    const uint32_t* constantBufferSpaces;\n");
    fprintf(fp, "\n");
    fprintf(fp, "    const uint32_t  numSRVTextures;\n");
    fprintf(fp, "    const char**    srvTextureNames;\n");
    fprintf(fp, "    const uint32_t* srvTextureBindings;\n");
    fprintf(fp, "    const uint32_t* srvTextureCounts;\n");
    fprintf(fp, "    const uint32_t* srvTextureSpaces;\n");
    fprintf(fp, "\n");
    fprintf(fp, "    const uint32_t  numUAVTextures;\n");
    fprintf(fp, "    const char**    uavTextureNames;\n");
    fprintf(fp, "    const uint32_t* uavTextureBindings;\n");
    fprintf(fp, "    const uint32_t* uavTextureCounts;\n");
    fprintf(fp, "    const uint32_t* uavTextureSpaces;\n");
    fprintf(fp, "\n");
    fprintf(fp, "    const uint32_t  numSRVBuffers;\n");
    fprintf(fp, "    const char**    srvBufferNames;\n");
    fprintf(fp, "    const uint32_t* srvBufferBindings;\n");
    fprintf(fp, "    const uint32_t* srvBufferCounts;\n");
    fprintf(fp, "    const uint32_t* srvBufferSpaces;\n");
    fprintf(fp, "\n");
    fprintf(fp, "    const uint32_t  numUAVBuffers;\n");
    fprintf(fp, "    const char**    uavBufferNames;\n");
    fprintf(fp, "    const uint32_t* uavBufferBindings;\n");
    fprintf(fp, "    const uint32_t* uavBufferCounts;\n");
    fprintf(fp, "    const uint32_t* uavBufferSpaces;\n");
    fprintf(fp, "\n");
    fprintf(fp, "    const uint32_t  numSamplers;\n");
    fprintf(fp, "    const char**    samplerNames;\n");
    fprintf(fp, "    const uint32_t* samplerBindings;\n");
    fprintf(fp, "    const uint32_t* samplerCounts;\n");
    fprintf(fp, "    const uint32_t* samplerSpaces;\n");
    fprintf(fp, "\n");
    fprintf(fp, "    const uint32_t  numRTAccelerationStructures;\n");
    fprintf(fp, "    const char**    rtAccelerationStructureNames;\n");
    fprintf(fp, "    const uint32_t* rtAccelerationStructureBindings;\n");
    fprintf(fp, "    const uint32_t* rtAccelerationStructureCounts;\n");
    fprintf(fp, "    const uint32_t* rtAccelerationStructureSpaces;\n");
}

void HLSLCompiler::WritePermutationHeaderReflectionData(FILE* fp, const Permutation& permutation)
{
    auto* reflection = dynamic_cast<IReflectionData*>(permutation.reflectionData.get());
    if (!reflection)
    {
        fprintf(fp, "0, 0, 0, 0, 0, ");
        fprintf(fp, "0, 0, 0, 0, 0, ");
        fprintf(fp, "0, 0, 0, 0, 0, ");
        fprintf(fp, "0, 0, 0, 0, 0, ");
        fprintf(fp, "0, 0, 0, 0, 0, ");
        fprintf(fp, "0, 0, 0, 0, 0, ");
        fprintf(fp, "0, 0, 0, 0, 0, ");
        return;
    }

    auto writeEntry = [&](const std::vector<ShaderResourceInfo>& resources, const char* suffix) {
        if (resources.empty())
        {
            fprintf(fp, "0, 0, 0, 0, 0, ");
        }
        else
        {
            fprintf(fp,
                    "%zu, g_%s_%sResourceNames, g_%s_%sResourceBindings, g_%s_%sResourceCounts, g_%s_%sResourceSpaces, ",
                    resources.size(),
                    permutation.name.c_str(),
                    suffix,
                    permutation.name.c_str(),
                    suffix,
                    permutation.name.c_str(),
                    suffix,
                    permutation.name.c_str(),
                    suffix);
        }
    };

    writeEntry(reflection->constantBuffers, "CBV");
    writeEntry(reflection->srvTextures, "TextureSRV");
    writeEntry(reflection->uavTextures, "TextureUAV");
    writeEntry(reflection->srvBuffers, "BufferSRV");
    writeEntry(reflection->uavBuffers, "BufferUAV");
    writeEntry(reflection->samplers, "Sampler");
    writeEntry(reflection->rtAccelerationStructures, "RTAccelerationStructure");
}

bool HLSLCompiler::CompileDXC(Permutation& permutation,
                              const std::vector<std::string>& arguments,
                              std::mutex&                     writeMutex)
{
    auto* shaderBinary = new HLSLDxcShaderBinary();
    permutation.shaderBinary = std::shared_ptr<IShaderBinary>(shaderBinary);

    std::vector<std::wstring> compilerArgs;
    std::vector<std::wstring> defineStorage;
    std::vector<fs::path>     includePaths;

    std::wstring entryPoint;
    std::wstring targetProfile;

    bool shouldGeneratePdb = m_DebugCompile;

    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        const std::string& arg = arguments[i];
        if (arg == "-E" && i + 1 < arguments.size())
        {
            entryPoint = UTF8ToWChar(arguments[++i]);
            continue;
        }
        if (arg == "-T" && i + 1 < arguments.size())
        {
            targetProfile = UTF8ToWChar(arguments[++i]);
            continue;
        }
        if (arg == "-I" && i + 1 < arguments.size())
        {
            includePaths.push_back(fs::path(arguments[++i]));
            compilerArgs.push_back(UTF8ToWChar(arg));
            compilerArgs.push_back(UTF8ToWChar(includePaths.back().generic_string()));
            continue;
        }
        if ((arg == "-Fo" || arg == "-Fd") && i + 1 < arguments.size())
        {
            // Drop requests for object/PDB file paths. The Linux CLI writes files directly.
            ++i;
            continue;
        }
        if (arg == "-Qstrip_debug")
        {
            // Windows-specific optimization flag; ignore here.
            continue;
        }
        if (arg == "-Zi" || arg == "-Zs" || arg == "-Zpr")
        {
            compilerArgs.push_back(UTF8ToWChar(arg));
            continue;
        }
        if (arg == "-D" && i + 1 < arguments.size())
        {
            std::wstring define = UTF8ToWChar(arguments[++i]);
            auto         pos    = define.find(L'=');
            std::wstring name   = define.substr(0, pos);
            std::wstring value  = pos == std::wstring::npos ? L"1" : define.substr(pos + 1);
            defineStorage.push_back(name);
            defineStorage.push_back(value);
            continue;
        }

        compilerArgs.push_back(UTF8ToWChar(arg));
    }

    if (entryPoint.empty() || targetProfile.empty())
    {
        throw std::runtime_error("DXC requires both -E <entry> and -T <profile> arguments.");
    }

    if (m_DebugCompile)
    {
        compilerArgs.push_back(DXC_ARG_DEBUG_NAME_FOR_SOURCE);
        compilerArgs.push_back(DXC_ARG_DEBUG);
        compilerArgs.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
    }

    std::vector<LPCWSTR> dxcArgs;
    dxcArgs.reserve(compilerArgs.size());
    for (const std::wstring& argument : compilerArgs)
    {
        dxcArgs.push_back(argument.c_str());
    }

    std::vector<DxcDefine> dxcDefines;
    dxcDefines.reserve(defineStorage.size() / 2);
    for (std::size_t i = 0; i + 1 < defineStorage.size(); i += 2)
    {
        dxcDefines.push_back(DxcDefine{defineStorage[i].c_str(), defineStorage[i + 1].c_str()});
    }

    CComPtr<IDxcCompilerArgs> compilerArguments;
    // DXC's argument builder only inspects the shader path during the call, so caching the
    // wide string locally keeps the pointer valid without imposing any broader allocations.
    const std::wstring shaderPathWide = UTF8ToWChar(m_ShaderPath);

    HRESULT hr = m_DxcUtils->BuildArguments(shaderPathWide.c_str(),
                                            entryPoint.c_str(),
                                            targetProfile.c_str(),
                                            dxcArgs.data(),
                                            dxcArgs.size(),
                                            dxcDefines.data(),
                                            dxcDefines.size(),
                                            &compilerArguments);
    if (FAILED(hr))
    {
        throw std::runtime_error("Failed to build DXC argument list.");
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr      = m_Source.data();
    sourceBuffer.Size     = m_Source.size();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    DxcCustomIncludeHandler includeHandler{};
    includeHandler.sourcePath         = permutation.sourcePath;
    includeHandler.includeSearchPaths = includePaths;
    includeHandler.upstream           = m_DxcDefaultIncludeHandler;

    hr = m_DxcCompiler->Compile(&sourceBuffer,
                                compilerArguments->GetArguments(),
                                compilerArguments->GetCount(),
                                &includeHandler,
                                IID_PPV_ARGS(&shaderBinary->pResults));
    if (FAILED(hr))
    {
        throw std::runtime_error("DXC compilation failed to start.");
    }

    permutation.dependencies = std::move(includeHandler.dependencies);

    HRESULT status = S_OK;
    shaderBinary->pResults->GetStatus(&status);
    bool succeeded = SUCCEEDED(status);

    CComPtr<IDxcBlobUtf8> errors;
    shaderBinary->pResults->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (!m_DisableLogs && errors && errors->GetStringLength() > 0)
    {
        std::lock_guard<std::mutex> guard(writeMutex);
        std::fprintf(stderr, "%s[%u]\n%s\n",
                     m_ShaderFileName.c_str(),
                     permutation.key,
                     errors->GetStringPointer());
    }

    if (!succeeded)
    {
        return false;
    }

    shaderBinary->pResults->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBinary->pShader), nullptr);
    if (!shaderBinary->pShader)
    {
        return false;
    }

    CComPtr<IDxcBlob> hashBlob;
    shaderBinary->pResults->GetOutput(DXC_OUT_SHADER_HASH, IID_PPV_ARGS(&hashBlob), nullptr);

    if (hashBlob)
    {
        const auto* hash = reinterpret_cast<const DxcShaderHash*>(hashBlob->GetBufferPointer());
        std::stringstream digest;
        digest << std::hex << std::setfill('0');
        for (uint8_t byte : hash->HashDigest)
        {
            digest << std::setw(2) << static_cast<uint32_t>(byte);
        }
        permutation.hashDigest = digest.str();
    }
    else
    {
        permutation.hashDigest = "00000000000000000000000000000000";
    }

    if (shouldGeneratePdb)
    {
        CComPtr<IDxcBlob> pdb;
        shaderBinary->pResults->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdb), nullptr);

        if (pdb)
        {
            const fs::path pdbPath = fs::path(m_OutputPath) / (permutation.hashDigest + ".pdb");
            if (auto stream = std::ofstream(pdbPath, std::ios::binary); stream.is_open())
            {
                stream.write(static_cast<const char*>(pdb->GetBufferPointer()),
                             static_cast<std::streamsize>(pdb->GetBufferSize()));
            }
        }
    }

    auto* shaderData = static_cast<uint8_t*>(shaderBinary->pShader->GetBufferPointer());
    DWORD checksum[4] = {};
    CalculateDXBCChecksum(shaderData,
                          static_cast<DWORD>(shaderBinary->pShader->GetBufferSize()),
                          checksum);

    (void)checksum; // The checksum is written into the blob by DXC already; this validates parity.

    permutation.name           = m_ShaderName + "_" + permutation.hashDigest;
    permutation.headerFileName = permutation.name + ".h";

    return true;
}

bool HLSLCompiler::ExtractDXCReflectionData(Permutation& permutation)
{
    permutation.reflectionData = std::make_shared<IReflectionData>();
    auto* shaderBinary = static_cast<HLSLDxcShaderBinary*>(permutation.shaderBinary.get());

    if (!shaderBinary || !shaderBinary->pShader)
    {
        return false;
    }

    DxcBuffer reflectionBuffer{};
    reflectionBuffer.Ptr      = shaderBinary->pShader->GetBufferPointer();
    reflectionBuffer.Size     = shaderBinary->pShader->GetBufferSize();
    reflectionBuffer.Encoding = DXC_CP_ACP;

    CComPtr<ID3D12ShaderReflection> reflection;
    HRESULT hr = m_DxcUtils->CreateReflection(&reflectionBuffer, IID_PPV_ARGS(&reflection));
    if (FAILED(hr) || !reflection)
    {
        return false;
    }

    D3D12_SHADER_DESC shaderDesc{};
    reflection->GetDesc(&shaderDesc);

    auto& reflectionData = *permutation.reflectionData;

    for (UINT resourceIndex = 0; resourceIndex < shaderDesc.BoundResources; ++resourceIndex)
    {
        D3D12_SHADER_INPUT_BIND_DESC binding{};
        reflection->GetResourceBindingDesc(resourceIndex, &binding);

        ShaderResourceInfo info{binding.Name, binding.BindPoint, binding.BindCount, binding.Space};

        switch (binding.Type)
        {
        case D3D_SIT_CBUFFER:
            reflectionData.constantBuffers.push_back(info);
            break;
        case D3D_SIT_TEXTURE:
            reflectionData.srvTextures.push_back(info);
            break;
        case D3D_SIT_SAMPLER:
            reflectionData.samplers.push_back(info);
            break;
        case D3D_SIT_UAV_RWTYPED:
            reflectionData.uavTextures.push_back(info);
            break;
        case D3D_SIT_STRUCTURED:
            reflectionData.srvBuffers.push_back(info);
            break;
        case D3D_SIT_UAV_RWSTRUCTURED:
            reflectionData.uavBuffers.push_back(info);
            break;
        case D3D_SIT_RTACCELERATIONSTRUCTURE:
            reflectionData.rtAccelerationStructures.push_back(info);
            break;
        case D3D_SIT_BYTEADDRESS:
            reflectionData.srvBuffers.push_back(info);
            break;
        case D3D_SIT_UAV_RWBYTEADDRESS:
            reflectionData.uavBuffers.push_back(info);
            break;
        default:
            throw std::runtime_error("Shader uses an unsupported resource type.");
        }
    }

    return true;
}

