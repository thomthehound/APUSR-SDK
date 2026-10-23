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

#pragma once

#include "ffx_sc/compiler.hpp"


/// The DXC (HLSL) specialization of <c><i>IShaderBinary</i></c> interface.
/// Handles everything necessary to export DXC compiled binary shader data.
///
/// @ingroup ShaderCompiler
struct HLSLDxcShaderBinary : public IShaderBinary
{
    CComPtr<IDxcResult> pResults;       ///< IDxcResult data from the shader compilation process for this shader binary
    CComPtr<IDxcBlob>   pShader;        ///< IDxcBlob shader blob data for this shader binary

    /// HLSL (DXC) Shader binary buffer accessor. 
    ///
    /// @returns
    /// Pointer to the internal HLSL (DXC) buffer representation.
    ///
    /// @ingroup ShaderCompiler
    uint8_t* BufferPointer() override;

    /// Queries the HLSL (DXC) shader binary size.
    ///
    /// @returns
    /// Size of the HLSL (DXC) shader binary
    ///
    /// @ingroup ShaderCompiler
    size_t   BufferSize() override;
};

/// The HLSLCompiler specialization of <c><i>ICompiler</i></c> interface.
/// Handles everything necessary to compile and extract shader reflection data
/// for HSLS and then exports the binary and reflection data for consumption
/// by HLSL-specific backends.
///
/// @ingroup ShaderCompiler
class HLSLCompiler : public ICompiler
{
public:

    /// Enumeration of possible HLSL backends to compile with.
    ///
    /// @ingroup ShaderCompiler
    enum Backend
    {
        DXC
    };

public:
    /// HLSL Compiler construction function
    /// 
    /// @param [in]  backend            The backend HLSL compile process to use
    /// @param [in]  dll                DXC/FXC DLL override to use when compiling
    /// @param [in]  shaderPath         Path to the shader to compile
    /// @param [in]  shaderName         Shader entry point
    /// @param [in]  shaderFileName     Filename of the shader file to compile
    /// @param [in]  outputPath         Output path for shader export
    /// @param [in]  disableLogs        Enables/Disables logging of errors and warnings
    /// @param [in]  debugCompile       Compile shaders in debug and generate pdb information
    ///
    /// @returns
    /// none
    ///
    /// @ingroup ShaderCompiler
    HLSLCompiler(Backend            backend,
                 const std::string& dll,
                 const std::string& shaderPath,
                 const std::string& shaderName,
                 const std::string& shaderFileName,
                 const std::string& outputPath,
                 bool               disableLogs,
                 bool               debugCompile);

    /// HLSL Compiler destruction function
    ///
    /// @returns
    /// none
    ///
    /// @ingroup ShaderCompiler
    ~HLSLCompiler();

    /// Compiles a HLSL shader permutation
    ///
    /// @param [in]  permutation            The permutation representation to compile
    /// @param [in]  arguments              List of arguments to pass to the compiler
    /// @param [in]  wrietMutex             Mutex to use for thread safety of compile process
    /// 
    /// @returns
    /// true if successful, false otherwise
    ///
    /// @ingroup ShaderCompiler
    bool Compile(Permutation&                    permutation,
                 const std::vector<std::string>& arguments,
                 std::mutex&                     writeMutex) override;

    /// Extracts HLSL shader reflection data
    ///
    /// @param [in]  permutation            The permutation representation to extract reflection for
    /// 
    /// @returns
    /// true if successful, false otherwise
    ///
    /// @ingroup ShaderCompiler
    bool ExtractReflectionData(Permutation& permutation) override;

    /// Writes HLSL reflection header data for shader permutations. 
    ///
    /// @param [in]  fp                     The file to write header information into
    /// @param [in]  permutation            The permutation representation to write to head
    /// @param [in]  wrietMutex             Mutex to use for thread safety of reflection data export
    /// 
    /// @returns
    /// none
    ///
    /// @ingroup ShaderCompiler
    void WriteBinaryHeaderReflectionData(FILE* fp, const Permutation& permutation, std::mutex& writeMutex) override;

    /// Writes HLSL permutation reflection header data structures for shader permutations. 
    ///
    /// @param [in]  fp                     The file to write header data structures into
    /// 
    /// @returns
    /// none
    ///
    /// @ingroup ShaderCompiler
    void WritePermutationHeaderReflectionStructMembers(FILE* fp) override;

    /// Writes HLSL permutation reflection header data for shader permutations. 
    ///
    /// @param [in]  fp                     The file to write header information into
    /// @param [in]  permutation            The permutation representation to write to head
    /// 
    /// @returns
    /// none
    ///
    /// @ingroup ShaderCompiler
    void WritePermutationHeaderReflectionData(FILE* fp, const Permutation& permutation) override;

private:
    bool CompileDXC(Permutation&                    permutation,
                    const std::vector<std::string>& arguments,
                    std::mutex&                     writeMutex);

    bool ExtractDXCReflectionData(Permutation& permutation);

private:
    Backend                     m_backend;
    std::string                 m_Source;

    // DXC backend
    CComPtr<IDxcUtils>          m_DxcUtils;
    CComPtr<IDxcCompiler3>      m_DxcCompiler;
    CComPtr<IDxcIncludeHandler> m_DxcDefaultIncludeHandler;
    DxcCreateInstanceProc       m_DxcCreateInstanceFunc;

};
