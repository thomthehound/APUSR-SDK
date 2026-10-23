#pragma once

#include <cstdint>

#include <dxc/WinAdapter.h>

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE __stdcall
#endif

#ifndef PURE
#define PURE = 0
#endif

#ifndef DECLSPEC_UUID
#define DECLSPEC_UUID(x)
#endif

// Forward declarations for reflection interfaces.
struct ID3D12ShaderReflectionType;
struct ID3D12ShaderReflectionVariable;
struct ID3D12ShaderReflectionConstantBuffer;
struct ID3D12LibraryReflection;
struct ID3D12FunctionReflection;
struct ID3D12FunctionParameterReflection;

// Minimal enumeration coverage mirroring d3dcommon.h values.
enum D3D_SHADER_INPUT_TYPE
{
    D3D_SIT_CBUFFER = 0,
    D3D_SIT_TBUFFER = 1,
    D3D_SIT_TEXTURE = 2,
    D3D_SIT_SAMPLER = 3,
    D3D_SIT_UAV_RWTYPED = 4,
    D3D_SIT_STRUCTURED = 5,
    D3D_SIT_UAV_RWSTRUCTURED = 6,
    D3D_SIT_BYTEADDRESS = 7,
    D3D_SIT_UAV_RWBYTEADDRESS = 8,
    D3D_SIT_UAV_APPEND_STRUCTURED = 9,
    D3D_SIT_UAV_CONSUME_STRUCTURED = 10,
    D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER = 11,
    D3D_SIT_RTACCELERATIONSTRUCTURE = 12,
    D3D_SIT_UAV_FEEDBACKTEXTURE = 13
};

enum D3D_RESOURCE_RETURN_TYPE
{
    D3D_RETURN_TYPE_UNORM = 1,
    D3D_RETURN_TYPE_SNORM = 2,
    D3D_RETURN_TYPE_SINT = 3,
    D3D_RETURN_TYPE_UINT = 4,
    D3D_RETURN_TYPE_FLOAT = 5,
    D3D_RETURN_TYPE_MIXED = 6,
    D3D_RETURN_TYPE_DOUBLE = 7,
    D3D_RETURN_TYPE_CONTINUED = 8
};

enum D3D_REGISTER_COMPONENT_TYPE
{
    D3D_REGISTER_COMPONENT_UNKNOWN = 0,
    D3D_REGISTER_COMPONENT_UINT32 = 1,
    D3D_REGISTER_COMPONENT_SINT32 = 2,
    D3D_REGISTER_COMPONENT_FLOAT32 = 3
};

enum D3D_MIN_PRECISION
{
    D3D_MIN_PRECISION_DEFAULT = 0,
    D3D_MIN_PRECISION_FLOAT_16 = 1,
    D3D_MIN_PRECISION_FLOAT_2_8 = 2,
    D3D_MIN_PRECISION_RESERVED = 3,
    D3D_MIN_PRECISION_SINT_16 = 4,
    D3D_MIN_PRECISION_UINT_16 = 5,
    D3D_MIN_PRECISION_ANY_16 = 0xf0,
    D3D_MIN_PRECISION_ANY_10 = 0xf1
};

enum D3D_SHADER_VARIABLE_CLASS
{
    D3D_SVC_SCALAR = 0,
    D3D_SVC_VECTOR = 1,
    D3D_SVC_MATRIX_ROWS = 2,
    D3D_SVC_MATRIX_COLUMNS = 3,
    D3D_SVC_OBJECT = 4,
    D3D_SVC_STRUCT = 5,
    D3D_SVC_INTERFACE_CLASS = 6,
    D3D_SVC_INTERFACE_POINTER = 7
};

enum D3D_SHADER_VARIABLE_TYPE
{
    D3D_SVT_VOID = 0,
    D3D_SVT_BOOL = 1,
    D3D_SVT_INT = 2,
    D3D_SVT_FLOAT = 3,
    D3D_SVT_STRING = 4,
    D3D_SVT_TEXTURE = 5,
    D3D_SVT_TEXTURE1D = 6,
    D3D_SVT_TEXTURE2D = 7,
    D3D_SVT_TEXTURE3D = 8,
    D3D_SVT_TEXTURECUBE = 9,
    D3D_SVT_SAMPLER = 10,
    D3D_SVT_PIXELSHADER = 15,
    D3D_SVT_VERTEXSHADER = 16,
    D3D_SVT_UINT = 19,
    D3D_SVT_TEXTURE1DARRAY = 20,
    D3D_SVT_TEXTURE2DARRAY = 21,
    D3D_SVT_RENDERTARGETVIEW = 60,
    D3D_SVT_DEPTHSTENCILVIEW = 61,
    D3D_SVT_BUFFER = 62,
    D3D_SVT_CBUFFER = 63,
    D3D_SVT_TBUFFER = 64,
    D3D_SVT_RWBUFFER = 65,
    D3D_SVT_RWSTRUCTUREDBUFFER = 67
};

enum D3D_SRV_DIMENSION
{
    D3D_SRV_DIMENSION_UNKNOWN = 0,
    D3D_SRV_DIMENSION_BUFFER = 1,
    D3D_SRV_DIMENSION_TEXTURE1D = 2,
    D3D_SRV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D_SRV_DIMENSION_TEXTURE2D = 4,
    D3D_SRV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D_SRV_DIMENSION_TEXTURE2DMS = 6,
    D3D_SRV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D_SRV_DIMENSION_TEXTURE3D = 8,
    D3D_SRV_DIMENSION_TEXTURECUBE = 9,
    D3D_SRV_DIMENSION_TEXTURECUBEARRAY = 10,
    D3D_SRV_DIMENSION_BUFFEREX = 11
};

using D3D_PRIMITIVE_TOPOLOGY = uint32_t;
using D3D_PRIMITIVE            = uint32_t;
using D3D_TESSELLATOR_DOMAIN   = uint32_t;
using D3D_TESSELLATOR_PARTITIONING = uint32_t;
using D3D_TESSELLATOR_OUTPUT_PRIMITIVE = uint32_t;
using D3D_FEATURE_LEVEL        = uint32_t;
using D3D_INTERPOLATION_MODE   = uint32_t;
using D3D_PARAMETER_FLAGS      = uint32_t;

typedef const char* LPCSTR;
typedef void*       LPVOID;

typedef enum D3D_NAME
{
    D3D_NAME_UNDEFINED = 0,
    D3D_NAME_POSITION = 1,
    D3D_NAME_CLIP_DISTANCE = 4,
    D3D_NAME_CULL_DISTANCE = 5,
    D3D_NAME_RENDER_TARGET_ARRAY_INDEX = 7,
    D3D_NAME_VIEWPORT_ARRAY_INDEX = 8,
    D3D_NAME_VERTEX_ID = 9,
    D3D_NAME_PRIMITIVE_ID = 10,
    D3D_NAME_INSTANCE_ID = 11,
    D3D_NAME_SAMPLE_INDEX = 13,
    D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR = 15,
    D3D_NAME_FINAL_QUAD_INSIDE_TESSFACTOR = 16,
    D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR = 17,
    D3D_NAME_FINAL_TRI_INSIDE_TESSFACTOR = 18,
    D3D_NAME_FINAL_LINE_DETAIL_TESSFACTOR = 19,
    D3D_NAME_FINAL_LINE_DENSITY_TESSFACTOR = 20,
    D3D_NAME_TARGET = 64,
    D3D_NAME_DEPTH = 65
} D3D_NAME;

struct D3D12_SHADER_INPUT_BIND_DESC
{
    LPCSTR                      Name;
    D3D_SHADER_INPUT_TYPE       Type;
    UINT                        BindPoint;
    UINT                        BindCount;
    UINT                        uFlags;
    D3D_RESOURCE_RETURN_TYPE    ReturnType;
    D3D_SRV_DIMENSION           Dimension;
    UINT                        NumSamples;
    UINT                        Space;
    UINT                        uID;
};

struct D3D12_SHADER_DESC
{
    UINT                    Version;
    LPCSTR                  Creator;
    UINT                    Flags;
    UINT                    ConstantBuffers;
    UINT                    BoundResources;
    UINT                    InputParameters;
    UINT                    OutputParameters;
    UINT                    InstructionCount;
    UINT                    TempRegisterCount;
    UINT                    TempArrayCount;
    UINT                    DefCount;
    UINT                    DclCount;
    UINT                    TextureNormalInstructions;
    UINT                    TextureLoadInstructions;
    UINT                    TextureCompInstructions;
    UINT                    TextureBiasInstructions;
    UINT                    TextureGradientInstructions;
    UINT                    FloatInstructionCount;
    UINT                    IntInstructionCount;
    UINT                    UintInstructionCount;
    UINT                    StaticFlowControlCount;
    UINT                    DynamicFlowControlCount;
    UINT                    MacroInstructionCount;
    UINT                    ArrayInstructionCount;
    UINT                    CutInstructionCount;
    UINT                    EmitInstructionCount;
    D3D_PRIMITIVE_TOPOLOGY  GSOutputTopology;
    UINT                    GSMaxOutputVertexCount;
    D3D_PRIMITIVE           InputPrimitive;
    UINT                    PatchConstantParameters;
    UINT                    cGSInstanceCount;
    UINT                    cControlPoints;
    D3D_TESSELLATOR_OUTPUT_PRIMITIVE HSOutputPrimitive;
    D3D_TESSELLATOR_PARTITIONING HSPartitioning;
    D3D_TESSELLATOR_DOMAIN  TessellatorDomain;
    UINT                    cBarrierInstructions;
    UINT                    cInterlockedInstructions;
    UINT                    cTextureStoreInstructions;
};

struct __declspec(uuid("5a58797d-a72c-478d-8b2f-ef1004187dda")) ID3D12ShaderReflection : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3D12_SHADER_DESC* pDesc) PURE;
    virtual ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByIndex(UINT Index) PURE;
    virtual ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByName(LPCSTR Name) PURE;
    virtual HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(UINT ResourceIndex, D3D12_SHADER_INPUT_BIND_DESC* pDesc) PURE;
    virtual HRESULT STDMETHODCALLTYPE GetInputParameterDesc(UINT ParameterIndex, void* pDesc) PURE;
    virtual HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(UINT ParameterIndex, void* pDesc) PURE;
    virtual HRESULT STDMETHODCALLTYPE GetPatchConstantParameterDesc(UINT ParameterIndex, void* pDesc) PURE;
    virtual ID3D12ShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) PURE;
    virtual HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(LPCSTR Name, D3D12_SHADER_INPUT_BIND_DESC* pDesc) PURE;
    virtual UINT STDMETHODCALLTYPE GetMovInstructionCount() PURE;
    virtual UINT STDMETHODCALLTYPE GetMovcInstructionCount() PURE;
    virtual UINT STDMETHODCALLTYPE GetConversionInstructionCount() PURE;
    virtual UINT STDMETHODCALLTYPE GetBitwiseInstructionCount() PURE;
    virtual D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetMinFeatureLevel() PURE;
    virtual UINT64 STDMETHODCALLTYPE GetRequiresFlags() PURE;
};

