// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_fsr4_reference_dx12.h>
#include <apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h>
#include <apusr/apusr_profiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
#if __has_include(<WinPixEventRuntime/pix3.h>)
#include <WinPixEventRuntime/pix3.h>
#elif __has_include(<pix3.h>)
#include <pix3.h>
#else
#error "APUSR_ENABLE_GPU_DEBUG_MARKERS requires WinPixEventRuntime pix3.h. Install Microsoft.WinPixEventRuntime or set APUSR_WINPIX_EVENT_RUNTIME_ROOT."
#endif
#endif

using Microsoft::WRL::ComPtr;
#endif

#ifndef APUSR_ENABLE_HOT_PATH_GUARDS
#define APUSR_ENABLE_HOT_PATH_GUARDS 0
#endif

#ifndef APUSR_ENABLE_GPU_DEBUG_MARKERS
#define APUSR_ENABLE_GPU_DEBUG_MARKERS 0
#endif

namespace apusr::backends {
namespace {

using namespace apusr::models::fsr4_int8;


#if defined(_WIN32)
constexpr std::size_t kDx12Fsr4ReferenceResourceLookupCount = static_cast<std::size_t>(ResourceId::ResultColor) + 1u;

std::size_t resourceLookupIndex(const ResourceId id) noexcept
{
    const std::size_t value = static_cast<std::size_t>(id);
    return value < kDx12Fsr4ReferenceResourceLookupCount ? value : 0u;
}
#endif

Dx12Fsr4ReferenceDescriptorSpace descriptorSpaceForBindingClass(const ReferenceBindingClass bindingClass)
{
    switch (bindingClass) {
    case ReferenceBindingClass::SrvTexture:
    case ReferenceBindingClass::SrvBuffer:
        return Dx12Fsr4ReferenceDescriptorSpace::Srv;
    case ReferenceBindingClass::UavTexture:
    case ReferenceBindingClass::UavBuffer:
        return Dx12Fsr4ReferenceDescriptorSpace::Uav;
    case ReferenceBindingClass::ConstantBuffer:
        return Dx12Fsr4ReferenceDescriptorSpace::Cbv;
    case ReferenceBindingClass::Sampler:
        return Dx12Fsr4ReferenceDescriptorSpace::Sampler;
    }
    return Dx12Fsr4ReferenceDescriptorSpace::Srv;
}

void updateRegisterCounts(
    const Dx12Fsr4ReferenceDescriptorSpace space,
    const std::uint32_t registerIndex,
    std::uint32_t& srv,
    std::uint32_t& uav,
    std::uint32_t& cbv,
    std::uint32_t& sampler)
{
    const std::uint32_t count = registerIndex + 1u;
    switch (space) {
    case Dx12Fsr4ReferenceDescriptorSpace::Srv:
        srv = std::max(srv, count);
        break;
    case Dx12Fsr4ReferenceDescriptorSpace::Uav:
        uav = std::max(uav, count);
        break;
    case Dx12Fsr4ReferenceDescriptorSpace::Cbv:
        cbv = std::max(cbv, count);
        break;
    case Dx12Fsr4ReferenceDescriptorSpace::Sampler:
        sampler = std::max(sampler, count);
        break;
    }
}

std::vector<Dx12Fsr4ReferenceDescriptorBinding> makeDx12DescriptorBindings(
    const ReferenceFramePlan& framePlan,
    std::uint32_t& outSrvCount,
    std::uint32_t& outUavCount,
    std::uint32_t& outCbvCount,
    std::uint32_t& outSamplerCount)
{
    std::size_t bindingCount = 0;
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (dispatch.enabled) {
            bindingCount += dispatch.bindings.size();
        }
    }

    std::vector<Dx12Fsr4ReferenceDescriptorBinding> bindings;
    bindings.reserve(bindingCount);

    std::uint32_t srv = 0;
    std::uint32_t uav = 0;
    std::uint32_t cbv = 0;
    std::uint32_t sampler = 0;
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        for (const ReferenceResourceBindingDesc& binding : dispatch.bindings) {
            const Dx12Fsr4ReferenceDescriptorSpace space = descriptorSpaceForBindingClass(binding.bindingClass);
            updateRegisterCounts(space, binding.shaderRegister, srv, uav, cbv, sampler);
            bindings.push_back(Dx12Fsr4ReferenceDescriptorBinding{
                .dispatchName = dispatch.name,
                .passKind = dispatch.kind,
                .logicalStage = dispatch.logicalStage,
                .space = space,
                .registerIndex = binding.shaderRegister,
                .registerSpace = binding.registerSpace,
                .resource = binding.resource,
                .constantBuffer = binding.constantBuffer,
                .access = binding.access,
                .shaderName = binding.shaderName,
                .optional = binding.optional,
            });
        }
    }

    outSrvCount = srv;
    outUavCount = uav;
    outCbvCount = cbv;
    outSamplerCount = sampler;
    return bindings;
}

ReferenceFramePlan makeDx12RootSignatureEnvelopeFramePlan(const ReferenceRunnerCreateDesc& createDesc)
{
    ReferenceRunnerFrameDesc frame{};
    frame.frame.renderSize = RenderSize{
        .width = std::max(1u, createDesc.maxRenderSize.width),
        .height = std::max(1u, createDesc.maxRenderSize.height),
    };
    frame.frame.displaySize = DisplaySize{
        .width = std::max(1u, createDesc.maxUpscaleSize.width),
        .height = std::max(1u, createDesc.maxUpscaleSize.height),
    };
    frame.frame.options.features = createDesc.features;
    frame.frame.options.features.debugVisualization = createDesc.allowDebugVisualization;
    frame.frame.options.hasDepth = true;
    frame.frame.options.hasExposure = true;
    frame.frame.options.sharpeningRequested = true;
    frame.residentModelPreset = createDesc.initialPreset;
    return makeReferenceFramePlan(createDesc, frame);
}

std::vector<Dx12Fsr4ReferenceShaderBuildDesc> makeDx12ShaderBuildPlan(
    const std::vector<ReferenceDispatchDesc>& dispatches)
{
    std::vector<Dx12Fsr4ReferenceShaderBuildDesc> shaders;
    shaders.reserve(dispatches.size());
    for (const ReferenceDispatchDesc& dispatch : dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        Dx12Fsr4ReferenceShaderBuildDesc shader{};
        shader.shader = dispatch.shader;
        shader.targetProfile = "cs_6_6";
        shader.hlslVersion = "2021";
        shader.enable16BitTypes = true;
        shaders.push_back(std::move(shader));
    }
    return shaders;
}

bool hasCbvRegister(
    const std::vector<Dx12Fsr4ReferenceDescriptorBinding>& bindings,
    const std::uint32_t registerIndex)
{
    for (const Dx12Fsr4ReferenceDescriptorBinding& binding : bindings) {
        if (binding.space == Dx12Fsr4ReferenceDescriptorSpace::Cbv && binding.registerIndex == registerIndex) {
            return true;
        }
    }
    return false;
}

[[maybe_unused]] std::string dispatchNameOrEmpty(const ReferenceDispatchDesc& dispatch)
{
    return dispatch.name != nullptr ? std::string(dispatch.name) : std::string{};
}

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path(path), error);
}

std::vector<std::uint8_t> readBinaryFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size <= 0) {
        return {};
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) {
        return {};
    }
    return bytes;
}

bool shaderProviderRequiresPackagedDispatch(
    const Dx12Fsr4ReferenceShaderProviderConfig& config,
    const char* dispatchName)
{
    if (dispatchName == nullptr || *dispatchName == '\0') {
        return false;
    }
    for (const std::string& required : config.requiredPackagedDispatches) {
        if (required == dispatchName) {
            return true;
        }
    }
    return false;
}

Dx12Fsr4ReferenceShaderResolutionDesc resolveShaderForDispatch(
    const Dx12Fsr4ReferenceShaderProviderConfig& config,
    const ReferenceFramePlan& framePlan,
    const ReferenceDispatchDesc& dispatch)
{
    Dx12Fsr4ReferenceShaderResolutionDesc resolution{};
    resolution.dispatchName = dispatch.name;
    resolution.cacheKey = dx12Fsr4ReferenceShaderCacheKey(
        framePlan.preset,
        framePlan.frameCapacity,
        framePlan.permutationKey,
        dispatch.name);

    resolution.requiredPackaged = shaderProviderRequiresPackagedDispatch(config, dispatch.name);

    if (config.enablePackagedCache && !config.packagedCacheRoot.empty()) {
        resolution.packagedPath = dx12Fsr4ReferencePackagedShaderBlobPath(
            config.packagedCacheRoot,
            framePlan.preset,
            framePlan.frameCapacity,
            framePlan.permutationKey,
            dispatch.name);
        if (fileExists(resolution.packagedPath)) {
            resolution.provider = Dx12Fsr4ReferenceShaderProviderKind::PackagedCache;
            resolution.available = true;
            return resolution;
        }
    }

    if (resolution.requiredPackaged) {
        resolution.provider = Dx12Fsr4ReferenceShaderProviderKind::PackagedCache;
        resolution.available = false;
        return resolution;
    }

    if (config.enableEmbeddedCache) {
        const auto* blob = fsr4_reference_dx12_shader_blobs::findShaderBlob(
            framePlan.preset,
            framePlan.frameCapacity,
            framePlan.permutationKey,
            dispatch.name);
        if (blob != nullptr && blob->data != nullptr && blob->size != 0) {
            resolution.provider = Dx12Fsr4ReferenceShaderProviderKind::EmbeddedCache;
            resolution.available = true;
            return resolution;
        }
    }

    resolution.provider = Dx12Fsr4ReferenceShaderProviderKind::RuntimeCompile;
    resolution.available = false;
    return resolution;
}

Dx12Fsr4ReferenceShaderProviderPlan makeShaderProviderPlan(
    const Dx12Fsr4ReferenceShaderProviderConfig& config,
    const ReferenceFramePlan& framePlan)
{
    Dx12Fsr4ReferenceShaderProviderPlan providerPlan{};
    providerPlan.config = config;
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        Dx12Fsr4ReferenceShaderResolutionDesc resolution = resolveShaderForDispatch(config, framePlan, dispatch);
        if (resolution.available) {
            switch (resolution.provider) {
            case Dx12Fsr4ReferenceShaderProviderKind::PackagedCache:
                ++providerPlan.packagedShaderBlobCount;
                break;
            case Dx12Fsr4ReferenceShaderProviderKind::EmbeddedCache:
                ++providerPlan.embeddedShaderBlobCount;
                break;
            case Dx12Fsr4ReferenceShaderProviderKind::RuntimeCompile:
                ++providerPlan.runtimeCompileCandidateCount;
                break;
            }
        } else {
            if (resolution.provider == Dx12Fsr4ReferenceShaderProviderKind::RuntimeCompile && config.allowRuntimeCompilation) {
                ++providerPlan.runtimeCompileCandidateCount;
            }
            ++providerPlan.missingShaderBlobCount;
        }
        providerPlan.shaders.push_back(std::move(resolution));
    }
    providerPlan.totalResolvedShaderBlobCount =
        providerPlan.packagedShaderBlobCount + providerPlan.embeddedShaderBlobCount;
    return providerPlan;
}

Status validateShaderProviderPlan(const Dx12Fsr4ReferenceBackendPlan& plan)
{
    if (plan.shaderProvider.missingShaderBlobCount == 0) {
        return Status::ok();
    }

    std::ostringstream message;
    message << "DX12 FSR4 reference backend is missing " << plan.shaderProvider.missingShaderBlobCount
            << " shader blob(s) for preset=" << presetName(plan.framePlan.preset)
            << " capacity=" << frameCapacityName(plan.framePlan.frameCapacity)
            << " permutationKey=" << plan.framePlan.permutationKey
            << "; product/runtime path requires a complete precompiled packaged shader cache";
    if (plan.shaderProvider.config.enableEmbeddedCache) {
        message << " or generated embedded product shader table";
    }
    if (plan.shaderProvider.config.allowRuntimeCompilation) {
        message << "; runtime compilation is disabled for reference validation; provide a complete shader cache";
    }
    for (const Dx12Fsr4ReferenceShaderResolutionDesc& shader : plan.shaderProvider.shaders) {
        if (!shader.available && shader.requiredPackaged) {
            message << "; required packaged dispatch=" << (shader.dispatchName != nullptr ? shader.dispatchName : "<null>");
            if (!shader.packagedPath.empty()) {
                message << " expected=" << shader.packagedPath;
            } else if (plan.shaderProvider.config.packagedCacheRoot.empty()) {
                message << " but packaged cache root is empty";
            }
        }
    }
    return Status::invalidArgument(message.str());
}

struct ResolvedShaderBytecode {
    Dx12Fsr4ReferenceShaderProviderKind provider = Dx12Fsr4ReferenceShaderProviderKind::EmbeddedCache;
    const char* dispatchName = nullptr;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::vector<std::uint8_t> ownedBytes;
};

[[maybe_unused]] Status resolveShaderBytecode(
    const Dx12Fsr4ReferenceShaderProviderConfig& config,
    const ReferenceFramePlan& framePlan,
    const ReferenceDispatchDesc& dispatch,
    ResolvedShaderBytecode& outBytecode)
{
    outBytecode = ResolvedShaderBytecode{};
    outBytecode.dispatchName = dispatch.name;

    const Dx12Fsr4ReferenceShaderResolutionDesc resolution = resolveShaderForDispatch(config, framePlan, dispatch);
    if (!resolution.available) {
        std::ostringstream message;
        message << "DX12 FSR4 reference shader is unavailable: key=" << resolution.cacheKey;
        if (resolution.requiredPackaged) {
            message << "; packaged cache is required for this dispatch";
            if (!resolution.packagedPath.empty()) {
                message << "; expected=" << resolution.packagedPath;
            } else if (config.packagedCacheRoot.empty()) {
                message << "; packaged cache root is empty";
            }
        }
        return Status::invalidArgument(message.str());
    }

    if (resolution.provider == Dx12Fsr4ReferenceShaderProviderKind::PackagedCache) {
        outBytecode.ownedBytes = readBinaryFile(resolution.packagedPath);
        if (outBytecode.ownedBytes.empty()) {
            std::ostringstream message;
            message << "DX12 FSR4 reference packaged shader cache entry could not be read: " << resolution.packagedPath;
            return Status::runtimeFailure(message.str());
        }
        outBytecode.provider = resolution.provider;
        outBytecode.data = outBytecode.ownedBytes.data();
        outBytecode.size = outBytecode.ownedBytes.size();
        return Status::ok();
    }

    if (resolution.provider == Dx12Fsr4ReferenceShaderProviderKind::EmbeddedCache) {
        const auto* blob = fsr4_reference_dx12_shader_blobs::findShaderBlob(
            framePlan.preset,
            framePlan.frameCapacity,
            framePlan.permutationKey,
            dispatch.name);
        if (blob == nullptr || blob->data == nullptr || blob->size == 0) {
            return Status::invalidArgument("DX12 FSR4 reference embedded product shader table lookup failed after resolution");
        }
        outBytecode.provider = resolution.provider;
        outBytecode.data = blob->data;
        outBytecode.size = blob->size;
        return Status::ok();
    }

    return Status::invalidArgument("DX12 FSR4 reference runtime shader compilation is disabled; provide a complete packaged shader cache or embedded product shader table");
}

[[maybe_unused]] std::string pipelineCacheKeyForDispatch(const ReferenceFramePlan& framePlan, const ReferenceDispatchDesc& dispatch)
{
    return dx12Fsr4ReferenceShaderCacheKey(
        framePlan.preset,
        framePlan.frameCapacity,
        framePlan.permutationKey,
        dispatch.name);
}

std::uint32_t addRootCbvRegisterIfPresent(
    Dx12Fsr4ReferenceRootSignaturePlan& plan,
    const std::vector<Dx12Fsr4ReferenceDescriptorBinding>& bindings,
    const std::uint32_t registerIndex,
    const ReferenceConstantBufferRole representativeRole)
{
    if (!hasCbvRegister(bindings, registerIndex)) {
        return std::numeric_limits<std::uint32_t>::max();
    }

    const std::uint32_t rootParameter = static_cast<std::uint32_t>(plan.rootParameters.size());
    plan.rootParameters.push_back(Dx12Fsr4ReferenceRootParameterDesc{
        .kind = Dx12Fsr4ReferenceRootParameterKind::RootCbv,
        .space = Dx12Fsr4ReferenceDescriptorSpace::Cbv,
        .rootParameterIndex = rootParameter,
        .baseShaderRegister = registerIndex,
        .registerSpace = 0,
        .descriptorCount = 1,
        .constantBuffer = representativeRole,
    });
    return rootParameter;
}


bool cstrEquals(const char* lhs, const char* rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return std::strcmp(lhs, rhs) == 0;
}

bool shaderNameCompatible(const char* plannedName, const char* reflectedName)
{
    return reflectedName == nullptr || plannedName == nullptr || cstrEquals(plannedName, reflectedName);
}

enum class JsonType : std::uint32_t {
    Null,
    Bool,
    Number,
    String,
    Object,
    Array,
};

struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolean = false;
    std::uint64_t number = 0;
    std::string string;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonValue& out)
    {
        skipWhitespace();
        if (!parseValue(out)) {
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            error_ = "unexpected trailing JSON content";
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& error() const { return error_; }

private:
    void skipWhitespace()
    {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(const char expected)
    {
        skipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != expected) {
            error_ = std::string("expected '") + expected + "'";
            return false;
        }
        ++pos_;
        return true;
    }

    bool parseValue(JsonValue& out)
    {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            error_ = "unexpected end of JSON input";
            return false;
        }
        const char c = text_[pos_];
        if (c == '"') {
            out.type = JsonType::String;
            return parseString(out.string);
        }
        if (c == '{') {
            return parseObject(out);
        }
        if (c == '[') {
            return parseArray(out);
        }
        if (c >= '0' && c <= '9') {
            return parseNumber(out);
        }
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out.type = JsonType::Bool;
            out.boolean = true;
            return true;
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out.type = JsonType::Bool;
            out.boolean = false;
            return true;
        }
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out = JsonValue{};
            return true;
        }
        error_ = "unexpected JSON value";
        return false;
    }

    bool parseString(std::string& out)
    {
        if (!consume('"')) {
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    error_ = "unterminated JSON string escape";
                    return false;
                }
                const char escaped = text_[pos_++];
                switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    // The generated manifest uses ASCII resource names.  Keep
                    // parsing deterministic without pulling a Unicode decoder
                    // into the backend; preserve the value as a placeholder.
                    if (pos_ + 4 > text_.size()) {
                        error_ = "short JSON unicode escape";
                        return false;
                    }
                    pos_ += 4;
                    out.push_back('?');
                    break;
                default:
                    error_ = "unsupported JSON string escape";
                    return false;
                }
            } else {
                out.push_back(c);
            }
        }
        error_ = "unterminated JSON string";
        return false;
    }

    bool parseNumber(JsonValue& out)
    {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(token.c_str(), &end, 10);
        if (end == token.c_str() || *end != '\0') {
            error_ = "invalid JSON integer";
            return false;
        }
        out.type = JsonType::Number;
        out.number = static_cast<std::uint64_t>(parsed);
        return true;
    }

    bool parseArray(JsonValue& out)
    {
        if (!consume('[')) {
            return false;
        }
        out = JsonValue{};
        out.type = JsonType::Array;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue element{};
            if (!parseValue(element)) {
                return false;
            }
            out.array.push_back(std::move(element));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    bool parseObject(JsonValue& out)
    {
        if (!consume('{')) {
            return false;
        }
        out = JsonValue{};
        out.type = JsonType::Object;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            if (!consume(':')) {
                return false;
            }
            JsonValue value{};
            if (!parseValue(value)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(value));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

const JsonValue* jsonField(const JsonValue& object, const char* name)
{
    if (object.type != JsonType::Object || name == nullptr) {
        return nullptr;
    }
    for (const auto& field : object.object) {
        if (field.first == name) {
            return &field.second;
        }
    }
    return nullptr;
}

bool jsonStringField(const JsonValue& object, const char* name, std::string& out, const bool required, std::string& error)
{
    const JsonValue* value = jsonField(object, name);
    if (value == nullptr) {
        if (required) {
            error = std::string("missing JSON string field '") + name + "'";
            return false;
        }
        out.clear();
        return true;
    }
    if (value->type != JsonType::String) {
        error = std::string("JSON field '") + name + "' must be a string";
        return false;
    }
    out = value->string;
    return true;
}

bool jsonUintField(const JsonValue& object, const char* name, std::uint32_t& out, const bool required, std::string& error)
{
    const JsonValue* value = jsonField(object, name);
    if (value == nullptr) {
        if (required) {
            error = std::string("missing JSON integer field '") + name + "'";
            return false;
        }
        out = 0;
        return true;
    }
    if (value->type != JsonType::Number || value->number > std::numeric_limits<std::uint32_t>::max()) {
        error = std::string("JSON field '") + name + "' must be a uint32 integer";
        return false;
    }
    out = static_cast<std::uint32_t>(value->number);
    return true;
}

bool presetFromManifestName(const std::string& name, Preset& out)
{
    if (name == "native") { out = Preset::NativeAA; return true; }
    if (name == "quality") { out = Preset::Quality; return true; }
    if (name == "balanced") { out = Preset::Balanced; return true; }
    if (name == "performance") { out = Preset::Performance; return true; }
    if (name == "drs") { out = Preset::DynamicResolution; return true; }
    if (name == "ultraperf") { out = Preset::UltraPerformance; return true; }
    return false;
}

bool frameCapacityFromManifestName(const std::string& name, FrameCapacity& out)
{
    if (name == "1080") { out = FrameCapacity::R1080; return true; }
    if (name == "2160") { out = FrameCapacity::R2160; return true; }
    if (name == "4320") { out = FrameCapacity::R4320; return true; }
    return false;
}

bool passKindFromManifestName(const std::string& name, PassKind& out)
{
    if (name == "spd-auto-exposure") { out = PassKind::SpdAutoExposure; return true; }
    if (name == "model-pre") { out = PassKind::ModelPre; return true; }
    if (name == "model-middle") { out = PassKind::ModelMiddle; return true; }
    if (name == "model-post") { out = PassKind::ModelPost; return true; }
    if (name == "rcas") { out = PassKind::Rcas; return true; }
    if (name == "debug-view") { out = PassKind::DebugView; return true; }
    return false;
}

bool descriptorSpaceFromManifestName(const std::string& name, Dx12Fsr4ReferenceDescriptorSpace& out)
{
    if (name == "srv") { out = Dx12Fsr4ReferenceDescriptorSpace::Srv; return true; }
    if (name == "uav") { out = Dx12Fsr4ReferenceDescriptorSpace::Uav; return true; }
    if (name == "cbv") { out = Dx12Fsr4ReferenceDescriptorSpace::Cbv; return true; }
    if (name == "sampler") { out = Dx12Fsr4ReferenceDescriptorSpace::Sampler; return true; }
    return false;
}

Status parseDx12Fsr4ReferenceShaderManifestRegisters(
    const JsonValue& resourcesValue,
    const std::string& context,
    std::vector<Dx12Fsr4ReferenceShaderReflectionManifestRegister>& outRegisters)
{
    if (resourcesValue.type != JsonType::Array) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest resources must be an array: " + context);
    }

    std::string error;
    outRegisters.clear();
    outRegisters.reserve(resourcesValue.array.size());
    for (const JsonValue& resourceValue : resourcesValue.array) {
        if (resourceValue.type != JsonType::Object) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest resources must be objects: " + context);
        }
        Dx12Fsr4ReferenceShaderReflectionManifestRegister reg{};
        std::string space;
        if (!jsonStringField(resourceValue, "name", reg.shaderName, true, error) ||
            !jsonStringField(resourceValue, "space", space, true, error)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest resource: " + error);
        }
        if (!descriptorSpaceFromManifestName(space, reg.space)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest resource has unknown descriptor space: " + space);
        }
        if (!jsonUintField(resourceValue, "register", reg.registerIndex, true, error)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest resource: " + error);
        }
        if (!jsonUintField(resourceValue, "registerSpace", reg.registerSpace, false, error)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest resource: " + error);
        }
        if (!jsonStringField(resourceValue, "type", reg.resourceType, false, error)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest resource: " + error);
        }
        outRegisters.push_back(std::move(reg));
    }
    return Status::ok();
}

bool manifestEntryMatchesFramePlan(
    const Dx12Fsr4ReferenceShaderReflectionManifestEntry& entry,
    const ReferenceFramePlan& framePlan)
{
    if (entry.preset != framePlan.preset || entry.frameCapacity != framePlan.frameCapacity) {
        return false;
    }
    if (entry.hasPermutationKey && entry.permutationKey != framePlan.permutationKey) {
        return false;
    }
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        if (dispatch.kind == entry.passKind && dispatch.logicalStage == entry.logicalStage && dispatch.name != nullptr &&
            entry.dispatchName == dispatch.name) {
            return true;
        }
    }
    return false;
}

std::uint64_t reflectionValidationCacheKeyForPlan(const ReferenceFramePlan& framePlan)
{
    std::uint64_t key = static_cast<std::uint64_t>(framePlan.permutationKey);
    key ^= static_cast<std::uint64_t>(presetPermutationValue(framePlan.preset)) << 32u;
    key ^= static_cast<std::uint64_t>(maxResolutionPermutationValue(framePlan.frameCapacity)) << 40u;
    key ^= static_cast<std::uint64_t>(framePlan.constants.rcasEnabled != 0 ? 1u : 0u) << 48u;
    key ^= static_cast<std::uint64_t>(framePlan.dispatches.size() & 0xFFu) << 56u;
    return key;
}

std::uint64_t modelResetKeyForPlan(const ReferenceFramePlan& framePlan)
{
    // Mirrors the FFX CreateModel() reset boundary: model preset, capacity,
    // and shader/model permutation changes require history reset.  Per-frame
    // debug-view drawing and RCAS do not belong in this key.
    std::uint64_t key = static_cast<std::uint64_t>(framePlan.permutationKey);
    key ^= static_cast<std::uint64_t>(presetPermutationValue(framePlan.preset)) << 32u;
    key ^= static_cast<std::uint64_t>(maxResolutionPermutationValue(framePlan.frameCapacity)) << 40u;
    return key;
}

std::uint32_t rootParameterForDescriptorTable(
    const Dx12Fsr4ReferenceRootSignaturePlan& plan,
    const Dx12Fsr4ReferenceDescriptorSpace space)
{
    if (space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
        return plan.srvTableRootParameter;
    }
    if (space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
        return plan.uavTableRootParameter;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t rootParameterForCbvRegister(
    const Dx12Fsr4ReferenceRootSignaturePlan& plan,
    const std::uint32_t registerIndex,
    const std::uint32_t registerSpace)
{
    for (const Dx12Fsr4ReferenceRootParameterDesc& parameter : plan.rootParameters) {
        if (parameter.kind == Dx12Fsr4ReferenceRootParameterKind::RootCbv &&
            parameter.baseShaderRegister == registerIndex &&
            parameter.registerSpace == registerSpace) {
            return parameter.rootParameterIndex;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t descriptorTableOffsetForRegister(
    const Dx12Fsr4ReferenceRootSignaturePlan& plan,
    const Dx12Fsr4ReferenceDescriptorSpace space,
    const std::uint32_t registerIndex,
    const std::uint32_t registerSpace)
{
    for (const Dx12Fsr4ReferenceRootParameterDesc& parameter : plan.rootParameters) {
        if ((parameter.kind == Dx12Fsr4ReferenceRootParameterKind::SrvDescriptorTable ||
             parameter.kind == Dx12Fsr4ReferenceRootParameterKind::UavDescriptorTable) &&
            parameter.space == space &&
            parameter.registerSpace == registerSpace &&
            registerIndex >= parameter.baseShaderRegister &&
            registerIndex < parameter.baseShaderRegister + parameter.descriptorCount) {
            return registerIndex - parameter.baseShaderRegister;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t shaderVisibleHeapOffsetForRegister(
    const Dx12Fsr4ReferenceRootSignaturePlan& plan,
    const Dx12Fsr4ReferenceDescriptorSpace space,
    const std::uint32_t descriptorTableOffset)
{
    if (descriptorTableOffset == std::numeric_limits<std::uint32_t>::max()) {
        return descriptorTableOffset;
    }
    if (space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
        return plan.descriptorHeap.srvBaseOffset + descriptorTableOffset;
    }
    if (space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
        return plan.descriptorHeap.uavBaseOffset + descriptorTableOffset;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

bool rootSignatureCoversRegister(
    const Dx12Fsr4ReferenceRootSignaturePlan& plan,
    const Dx12Fsr4ReferenceShaderRegisterUse& use)
{
    if (use.space == Dx12Fsr4ReferenceDescriptorSpace::Srv || use.space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
        return descriptorTableOffsetForRegister(plan, use.space, use.registerIndex, use.registerSpace) !=
            std::numeric_limits<std::uint32_t>::max();
    }
    if (use.space == Dx12Fsr4ReferenceDescriptorSpace::Cbv) {
        return rootParameterForCbvRegister(plan, use.registerIndex, use.registerSpace) !=
            std::numeric_limits<std::uint32_t>::max();
    }
    if (use.space == Dx12Fsr4ReferenceDescriptorSpace::Sampler) {
        for (const Dx12Fsr4ReferenceStaticSamplerDesc& sampler : plan.staticSamplers) {
            if (sampler.shaderRegister == use.registerIndex && sampler.registerSpace == use.registerSpace &&
                shaderNameCompatible(sampler.shaderName, use.shaderName)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

bool descriptorBindingAppliesToShader(
    const Dx12Fsr4ReferenceDescriptorBinding& binding,
    const Dx12Fsr4ReferenceShaderReflectionDesc& shader);

bool bindingPlanCoversRegister(
    const Dx12Fsr4ReferenceBackendPlan& plan,
    const Dx12Fsr4ReferenceShaderReflectionDesc& shader,
    const Dx12Fsr4ReferenceShaderRegisterUse& use)
{
    for (const Dx12Fsr4ReferenceDescriptorBinding& binding : plan.descriptorBindings) {
        if (!descriptorBindingAppliesToShader(binding, shader)) {
            continue;
        }
        if (binding.space == use.space &&
            binding.registerIndex == use.registerIndex &&
            binding.registerSpace == use.registerSpace &&
            shaderNameCompatible(binding.shaderName, use.shaderName)) {
            return true;
        }
    }
    return false;
}

bool framePlanHasDispatch(const ReferenceFramePlan& framePlan, const Dx12Fsr4ReferenceShaderReflectionDesc& shader)
{
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (dispatch.enabled && dispatch.kind == shader.passKind && cstrEquals(dispatch.name, shader.dispatchName)) {
            return true;
        }
    }
    return false;
}

bool descriptorBindingAppliesToShader(
    const Dx12Fsr4ReferenceDescriptorBinding& binding,
    const Dx12Fsr4ReferenceShaderReflectionDesc& shader)
{
    return binding.passKind == shader.passKind &&
        binding.logicalStage == shader.logicalStage &&
        cstrEquals(binding.dispatchName, shader.dispatchName);
}

bool patchDescMatchesDispatchName(const Dx12Fsr4ReferenceDescriptorPatchDesc& patch, const ReferenceDispatchDesc& dispatch)
{
    return patch.dispatchName != nullptr &&
           dispatch.name != nullptr &&
           std::strcmp(patch.dispatchName, dispatch.name) == 0;
}

void addTunedTransition(
    Dx12Fsr4ReferenceDispatchRecordPlan& record,
    const Dx12Fsr4ReferenceDescriptorPatchDesc& patch)
{
    if (patch.resolvedResource == ResourceId::Null ||
        (patch.space != Dx12Fsr4ReferenceDescriptorSpace::Srv && patch.space != Dx12Fsr4ReferenceDescriptorSpace::Uav)) {
        return;
    }
    for (Dx12Fsr4ReferenceDispatchResourceTransitionDesc& transition : record.resourceTransitions) {
        if (transition.resource != patch.resolvedResource) {
            continue;
        }
        if (patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
            transition.descriptorSpace = Dx12Fsr4ReferenceDescriptorSpace::Uav;
        }
        transition.optional = transition.optional && patch.optional;
        return;
    }
    record.resourceTransitions.push_back(Dx12Fsr4ReferenceDispatchResourceTransitionDesc{
        .resource = patch.resolvedResource,
        .descriptorSpace = patch.space,
        .optional = patch.optional,
    });
}

std::vector<Dx12Fsr4ReferenceDispatchRecordPlan> makeDispatchRecordPlan(
    const ReferenceFramePlan& framePlan,
    const std::vector<Dx12Fsr4ReferenceDescriptorPatchDesc>& descriptorPatches,
    const Dx12Fsr4ReferenceRootSignaturePlan& rootSignature)
{
    std::vector<Dx12Fsr4ReferenceDispatchRecordPlan> records;
    records.reserve(framePlan.dispatches.size());
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        Dx12Fsr4ReferenceDispatchRecordPlan record{};
        record.dispatchName = dispatch.name;
        record.passKind = dispatch.kind;
        record.logicalStage = dispatch.logicalStage;
        record.grid = dispatch.grid;
        record.primaryConstants = ReferenceConstantBufferRole::None;
        record.primaryConstantsRootParameter = std::numeric_limits<std::uint32_t>::max();
        record.passWeightsRootParameter = std::numeric_limits<std::uint32_t>::max();
        record.pipelineCacheKey = pipelineCacheKeyForDispatch(framePlan, dispatch);
        for (const ReferenceResourceBindingDesc& binding : dispatch.bindings) {
            const Dx12Fsr4ReferenceDescriptorSpace space = descriptorSpaceForBindingClass(binding.bindingClass);
            switch (space) {
            case Dx12Fsr4ReferenceDescriptorSpace::Srv:
                ++record.srvBindingCount;
                break;
            case Dx12Fsr4ReferenceDescriptorSpace::Uav:
                ++record.uavBindingCount;
                break;
            case Dx12Fsr4ReferenceDescriptorSpace::Cbv: {
                ++record.cbvBindingCount;
                const std::uint32_t rootParameter = rootParameterForCbvRegister(rootSignature, binding.shaderRegister, binding.registerSpace);
                record.constantBuffers.push_back(Dx12Fsr4ReferenceDispatchCbvBinding{
                    .role = binding.constantBuffer,
                    .shaderRegister = binding.shaderRegister,
                    .registerSpace = binding.registerSpace,
                    .rootParameterIndex = rootParameter,
                    .shaderName = binding.shaderName,
                });
                if (record.primaryConstants == ReferenceConstantBufferRole::None) {
                    record.primaryConstants = binding.constantBuffer;
                    record.primaryConstantsRootParameter = rootParameter;
                }
                if (binding.constantBuffer == ReferenceConstantBufferRole::PassWeights) {
                    record.passWeightsRootParameter = rootParameter;
                }
                break;
            }
            case Dx12Fsr4ReferenceDescriptorSpace::Sampler:
                ++record.samplerBindingCount;
                break;
            }
        }
        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : descriptorPatches) {
            if (!patchDescMatchesDispatchName(patch, dispatch)) {
                continue;
            }
            if (patch.space == Dx12Fsr4ReferenceDescriptorSpace::Srv &&
                record.srvDescriptorTableBase == std::numeric_limits<std::uint32_t>::max() &&
                patch.shaderVisibleHeapOffset != std::numeric_limits<std::uint32_t>::max() &&
                patch.descriptorTableOffset != std::numeric_limits<std::uint32_t>::max()) {
                record.srvDescriptorTableBase = patch.shaderVisibleHeapOffset - patch.descriptorTableOffset;
            }
            if (patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav &&
                record.uavDescriptorTableBase == std::numeric_limits<std::uint32_t>::max() &&
                patch.shaderVisibleHeapOffset != std::numeric_limits<std::uint32_t>::max() &&
                patch.descriptorTableOffset != std::numeric_limits<std::uint32_t>::max()) {
                record.uavDescriptorTableBase = patch.shaderVisibleHeapOffset - patch.descriptorTableOffset;
            }
            addTunedTransition(record, patch);
        }
        records.push_back(record);
    }
    return records;
}

std::vector<Dx12Fsr4ReferenceDescriptorPatchDesc> makeDescriptorPatchPlan(
    const ResourcePlan& resourcePlan,
    const ReferenceFramePlan& framePlan,
    const Dx12Fsr4ReferenceRootSignaturePlan& rootSignature)
{
    const bool rcasEnabled = framePlan.constants.rcasEnabled != 0;
    const std::uint32_t perDispatchDescriptorStride = rootSignature.descriptorHeap.totalCbvSrvUavDescriptors;
    std::uint32_t dispatchDescriptorSegment = 0;
    std::vector<Dx12Fsr4ReferenceDescriptorPatchDesc> patches;
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        const std::uint32_t dispatchDescriptorBase = dispatchDescriptorSegment * perDispatchDescriptorStride;
        ++dispatchDescriptorSegment;
        for (const ReferenceResourceBindingDesc& binding : dispatch.bindings) {
            const Dx12Fsr4ReferenceDescriptorSpace space = descriptorSpaceForBindingClass(binding.bindingClass);
            Dx12Fsr4ReferenceDescriptorPatchDesc patch{};
            patch.dispatchName = dispatch.name;
            patch.passKind = dispatch.kind;
            patch.logicalStage = dispatch.logicalStage;
            patch.space = space;
            patch.registerIndex = binding.shaderRegister;
            patch.registerSpace = binding.registerSpace;
            patch.resource = binding.resource;
            patch.resolvedResource = dx12Fsr4ReferenceResolveAlias(resourcePlan, binding.resource, rcasEnabled);
            if (binding.resource == ResourceId::InputExposure && framePlan.constants.useDefaultExposure != 0) {
                patch.resolvedResource = ResourceId::InternalDefaultExposure;
            }
            patch.constantBuffer = binding.constantBuffer;
            patch.access = binding.access;
            patch.shaderName = binding.shaderName;
            patch.optional = binding.optional;
            if (const ResourceDesc* resolved = findResource(resourcePlan, patch.resolvedResource)) {
                patch.externalBorrowed = resolved->lifetime == ResourceLifetime::External;
            }
            if (space == Dx12Fsr4ReferenceDescriptorSpace::Srv || space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
                patch.rootParameterIndex = rootParameterForDescriptorTable(rootSignature, space);
                patch.descriptorTableOffset = descriptorTableOffsetForRegister(rootSignature, space, binding.shaderRegister, binding.registerSpace);
                const std::uint32_t tableOffset = shaderVisibleHeapOffsetForRegister(rootSignature, space, patch.descriptorTableOffset);
                patch.shaderVisibleHeapOffset = tableOffset == std::numeric_limits<std::uint32_t>::max()
                    ? tableOffset
                    : dispatchDescriptorBase + tableOffset;
            } else if (space == Dx12Fsr4ReferenceDescriptorSpace::Cbv) {
                patch.rootParameterIndex = rootParameterForCbvRegister(rootSignature, binding.shaderRegister, binding.registerSpace);
            }
            patches.push_back(patch);
        }
    }
    return patches;
}


NativeHandle dx12ExternalHandleForResource(const Dx12Fsr4ReferenceFrameDesc& frameDesc, const ResourceId resource)
{
    switch (resource) {
    case ResourceId::InputColor:
        return frameDesc.externalResources.inputColor;
    case ResourceId::InputDepth:
        return frameDesc.externalResources.inputDepth;
    case ResourceId::InputMotionVectors:
        return frameDesc.externalResources.inputMotionVectors;
    case ResourceId::InputExposure:
        return frameDesc.externalResources.inputExposure;
    case ResourceId::FinalOutput:
        return frameDesc.externalResources.output;
    default:
        return NativeHandle{};
    }
}

bool resourceUsedBySelectedFrame(
    const std::vector<ReferenceResourceUsageDesc>& usagePlan,
    const ResourceId resource)
{
    if (resource == ResourceId::Null) {
        return false;
    }
    if (resource == ResourceId::FinalOutput) {
        return true;
    }
    for (const ReferenceResourceUsageDesc& usage : usagePlan) {
        if (usage.resource == resource) {
            return true;
        }
    }
    return false;
}

bool resourceUsedBySelectedFrame(const ReferenceFramePlan& framePlan, const ResourceId resource)
{
    return resourceUsedBySelectedFrame(makeReferenceResourceUsagePlan(framePlan), resource);
}

bool externalResourceRequiredForSelectedFrame(
    const ReferenceFramePlan& framePlan,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc,
    const ResourceDesc& resource)
{
    if (resource.lifetime != ResourceLifetime::External) {
        return false;
    }
    if (resource.id == ResourceId::InputExposure) {
        return frameDesc.runner.frame.options.hasExposure && framePlan.constants.useDefaultExposure == 0;
    }
    if (resource.id == ResourceId::InputDepth) {
        return frameDesc.runner.frame.options.hasDepth && resourceUsedBySelectedFrame(framePlan, resource.id);
    }
    return resourceUsedBySelectedFrame(framePlan, resource.id);
}

std::uint64_t resourceCommittedByteEstimate(const ResourceDesc& resource)
{
    if (resource.kind == ResourceKind::Buffer) {
        return resource.byteSize;
    }
    std::uint64_t bytesPerPixel = 0;
    switch (resource.format) {
    case SurfaceFormat::Rgba32Float:
    case SurfaceFormat::Rgba32Uint:
        bytesPerPixel = 16;
        break;
    case SurfaceFormat::Rgba16Float:
    case SurfaceFormat::Rg32Float:
    case SurfaceFormat::Rg32Uint:
        bytesPerPixel = 8;
        break;
    case SurfaceFormat::Rgb32Float:
        bytesPerPixel = 12;
        break;
    case SurfaceFormat::Rg16Float:
    case SurfaceFormat::Rg16Uint:
    case SurfaceFormat::Rg16Sint:
    case SurfaceFormat::R32Float:
    case SurfaceFormat::R32Uint:
    case SurfaceFormat::Rgba8Unorm:
    case SurfaceFormat::Rgba8Snorm:
    case SurfaceFormat::Rgba8Srgb:
    case SurfaceFormat::Bgra8Unorm:
    case SurfaceFormat::Bgra8Srgb:
    case SurfaceFormat::R11G11B10Float:
    case SurfaceFormat::R10G10B10A2Unorm:
        bytesPerPixel = 4;
        break;
    case SurfaceFormat::Rg8Unorm:
    case SurfaceFormat::Rg8Uint:
    case SurfaceFormat::R16Float:
    case SurfaceFormat::R16Uint:
    case SurfaceFormat::R16Unorm:
    case SurfaceFormat::R16Snorm:
        bytesPerPixel = 2;
        break;
    case SurfaceFormat::R8Unorm:
    case SurfaceFormat::R8Uint:
    case SurfaceFormat::R8Sint:
    case SurfaceFormat::R8Snorm:
        bytesPerPixel = 1;
        break;
    case SurfaceFormat::Unknown:
        bytesPerPixel = 0;
        break;
    }
    return static_cast<std::uint64_t>(resource.width) * static_cast<std::uint64_t>(resource.height) * bytesPerPixel;
}

#if defined(_WIN32)
std::uint64_t resourceCommittedByteEstimate(const Dx12Fsr4ReferenceRealizedResourceDesc& resource)
{
    if (resource.kind == ResourceKind::Buffer) {
        return resource.byteSize;
    }
    ResourceDesc desc{};
    desc.kind = resource.kind;
    desc.format = resource.format;
    desc.width = resource.width;
    desc.height = resource.height;
    desc.byteSize = resource.byteSize;
    return resourceCommittedByteEstimate(desc);
}

#endif

std::uint64_t resourceCacheKeyForPlan(const ResourcePlan& plan) noexcept
{
    std::uint64_t key = 0x4150555352465352ull;
    key = apusr::profile::hashCombine(key, static_cast<std::uint64_t>(plan.frameCapacity));
    key = apusr::profile::hashCombine(key,
        (static_cast<std::uint64_t>(plan.maxRenderSize.width) << 32u) | plan.maxRenderSize.height);
    key = apusr::profile::hashCombine(key,
        (static_cast<std::uint64_t>(plan.alignedMaxUpscaleSize.width) << 32u) | plan.alignedMaxUpscaleSize.height);
    key = apusr::profile::hashCombine(key, plan.initializerByteSize);
    key = apusr::profile::hashCombine(key, plan.scratchByteSize);
    key = apusr::profile::hashCombine(key, findResource(plan, ResourceId::DebugInformation) != nullptr ? 1u : 0u);
    return key;
}

std::uint64_t alignUpU64(const std::uint64_t value, const std::uint64_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1u) / alignment) * alignment;
}

std::uint64_t constantUploadByteSize(const ReferenceConstantBufferRole role)
{
    switch (role) {
    case ReferenceConstantBufferRole::Fsr4Upscaler:
        return sizeof(OptimizedConstants);
    case ReferenceConstantBufferRole::SpdAutoExposure:
        return sizeof(AutoExposureSpdConstants);
    case ReferenceConstantBufferRole::Rcas:
        return sizeof(RcasConstants);
    case ReferenceConstantBufferRole::PassWeights:
        return sizeof(Pass0WeightsConstants);
    case ReferenceConstantBufferRole::None:
        break;
    }
    return 0;
}

bool framePlanUsesConstantRole(const ReferenceFramePlan& framePlan, const ReferenceConstantBufferRole role)
{
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        for (const ReferenceResourceBindingDesc& binding : dispatch.bindings) {
            if (binding.bindingClass == ReferenceBindingClass::ConstantBuffer && binding.constantBuffer == role) {
                return true;
            }
        }
    }
    return false;
}

std::uint32_t rootParameterForConstantRole(
    const Dx12Fsr4ReferenceRootSignaturePlan& rootSignature,
    const ReferenceConstantBufferRole role)
{
    switch (role) {
    case ReferenceConstantBufferRole::Fsr4Upscaler:
        return rootSignature.fsr4ConstantsRootParameter;
    case ReferenceConstantBufferRole::SpdAutoExposure:
        return rootSignature.spdConstantsRootParameter;
    case ReferenceConstantBufferRole::Rcas:
        return rootSignature.rcasConstantsRootParameter;
    case ReferenceConstantBufferRole::PassWeights:
        return rootSignature.passWeightsRootParameter;
    case ReferenceConstantBufferRole::None:
        break;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t shaderRegisterForConstantRole(const ReferenceConstantBufferRole role)
{
    return role == ReferenceConstantBufferRole::PassWeights ? 1u : 0u;
}

Dx12Fsr4ReferenceConstantUploadPlan makeConstantUploadPlan(
    const ReferenceFramePlan& framePlan,
    const Dx12Fsr4ReferenceRootSignaturePlan& rootSignature)
{
    Dx12Fsr4ReferenceConstantUploadPlan plan{};

    const std::array<ReferenceConstantBufferRole, 4> order{
        ReferenceConstantBufferRole::Fsr4Upscaler,
        ReferenceConstantBufferRole::SpdAutoExposure,
        ReferenceConstantBufferRole::Rcas,
        ReferenceConstantBufferRole::PassWeights,
    };

    std::uint64_t cursor = 0;
    for (const ReferenceConstantBufferRole role : order) {
        if (!framePlanUsesConstantRole(framePlan, role)) {
            continue;
        }
        const std::uint64_t size = constantUploadByteSize(role);
        if (size == 0) {
            continue;
        }
        cursor = alignUpU64(cursor, 256);
        plan.slots.push_back(Dx12Fsr4ReferenceConstantUploadSlot{
            .role = role,
            .rootParameterIndex = rootParameterForConstantRole(rootSignature, role),
            .shaderRegister = shaderRegisterForConstantRole(role),
            .registerSpace = 0,
            .byteOffset = cursor,
            .byteSize = size,
            .debugName = referenceConstantBufferRoleName(role),
        });
        if (role == ReferenceConstantBufferRole::PassWeights) {
            plan.hasPassWeights = true;
        } else if (role == ReferenceConstantBufferRole::SpdAutoExposure) {
            plan.hasSpdConstants = true;
        } else if (role == ReferenceConstantBufferRole::Rcas) {
            plan.hasRcasConstants = true;
        }
        cursor += size;
    }

    plan.perFrameByteSize = alignUpU64(cursor, 256);
    plan.slotCount = static_cast<std::uint32_t>(plan.slots.size());
    return plan;
}


const Dx12Fsr4ReferenceConstantUploadSlot* constantUploadSlotForRole(
    const Dx12Fsr4ReferenceConstantUploadPlan& plan,
    const ReferenceConstantBufferRole role)
{
    for (const Dx12Fsr4ReferenceConstantUploadSlot& slot : plan.slots) {
        if (slot.role == role) {
            return &slot;
        }
    }
    return nullptr;
}

void populateDispatchRecordConstantOffsets(
    std::vector<Dx12Fsr4ReferenceDispatchRecordPlan>& records,
    const Dx12Fsr4ReferenceConstantUploadPlan& constantPlan)
{
    for (Dx12Fsr4ReferenceDispatchRecordPlan& record : records) {
        for (Dx12Fsr4ReferenceDispatchCbvBinding& cbv : record.constantBuffers) {
            if (const Dx12Fsr4ReferenceConstantUploadSlot* slot = constantUploadSlotForRole(constantPlan, cbv.role)) {
                cbv.byteOffset = slot->byteOffset;
            }
        }
    }
}

const Dx12Fsr4ReferenceRealizedResourceDesc* findRealizedResource(
    const Dx12Fsr4ReferenceResourceRealizationPlan& plan,
    const ResourceId id)
{
    for (const Dx12Fsr4ReferenceRealizedResourceDesc& resource : plan.resources) {
        if (resource.id == id) {
            return &resource;
        }
    }
    return nullptr;
}

Dx12Fsr4ReferenceDescriptorRealizationPlan makeDescriptorRealizationPlan(
    const Dx12Fsr4ReferenceRootSignaturePlan& rootSignature,
    const Dx12Fsr4ReferenceResourceRealizationPlan& resources,
    const std::vector<Dx12Fsr4ReferenceDescriptorPatchDesc>& patches)
{
    Dx12Fsr4ReferenceDescriptorRealizationPlan plan{};

    std::set<std::uint32_t> uniqueSlots;
    std::uint32_t maxShaderVisibleSlot = 0;
    for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : patches) {
        if (patch.space != Dx12Fsr4ReferenceDescriptorSpace::Srv &&
            patch.space != Dx12Fsr4ReferenceDescriptorSpace::Uav) {
            continue;
        }
        if (patch.space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
            ++plan.activeSrvPatchCount;
        } else {
            ++plan.activeUavPatchCount;
        }
        if (patch.shaderVisibleHeapOffset != std::numeric_limits<std::uint32_t>::max()) {
            uniqueSlots.insert(patch.shaderVisibleHeapOffset);
            maxShaderVisibleSlot = std::max(maxShaderVisibleSlot, patch.shaderVisibleHeapOffset + 1u);
        }
        const Dx12Fsr4ReferenceRealizedResourceDesc* resource = findRealizedResource(resources, patch.resolvedResource);
        if (!patch.optional && (resource == nullptr || !resource->validHandle)) {
            ++plan.missingRequiredResourceDescriptorCount;
        }
    }
    plan.uniqueShaderVisibleSlotCount = static_cast<std::uint32_t>(uniqueSlots.size());
    if (rootSignature.descriptorHeap.totalCbvSrvUavDescriptors != 0 && maxShaderVisibleSlot != 0) {
        plan.shaderVisibleDescriptorCount = alignUp(maxShaderVisibleSlot, rootSignature.descriptorHeap.totalCbvSrvUavDescriptors);
    } else {
        plan.shaderVisibleDescriptorCount = maxShaderVisibleSlot;
    }
    plan.heapRequired = plan.shaderVisibleDescriptorCount != 0;
    return plan;
}

void refreshResourceRealizationPerFrame(
    const ReferenceFramePlan& framePlan,
    const std::vector<ReferenceResourceUsageDesc>& usagePlan,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc,
    Dx12Fsr4ReferenceResourceRealizationPlan& realization)
{
    realization.missingRequiredExternalCount = 0;
    realization.missingOptionalExternalCount = 0;

    for (Dx12Fsr4ReferenceRealizedResourceDesc& resource : realization.resources) {
        if (resource.ownership != Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed) {
            continue;
        }

        const bool usedByFrame = resourceUsedBySelectedFrame(usagePlan, resource.id);
        if (resource.id == ResourceId::InputExposure) {
            resource.requiredForSelectedFrame =
                frameDesc.runner.frame.options.hasExposure && framePlan.constants.useDefaultExposure == 0u;
        } else if (resource.id == ResourceId::InputDepth) {
            resource.requiredForSelectedFrame = frameDesc.runner.frame.options.hasDepth && usedByFrame;
        } else {
            resource.requiredForSelectedFrame = usedByFrame;
        }

        resource.nativeResource = dx12ExternalHandleForResource(frameDesc, resource.id);
        resource.validHandle = resource.nativeResource.value != nullptr;
        if (!resource.validHandle) {
            if (resource.requiredForSelectedFrame) {
                ++realization.missingRequiredExternalCount;
            } else {
                ++realization.missingOptionalExternalCount;
            }
        }
    }
}

void refreshDescriptorRealizationPerFrame(
    const Dx12Fsr4ReferenceResourceRealizationPlan& resources,
    const std::vector<Dx12Fsr4ReferenceDescriptorPatchDesc>& patches,
    Dx12Fsr4ReferenceDescriptorRealizationPlan& realization)
{
    realization.missingRequiredResourceDescriptorCount = 0;
    for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : patches) {
        if (patch.space != Dx12Fsr4ReferenceDescriptorSpace::Srv &&
            patch.space != Dx12Fsr4ReferenceDescriptorSpace::Uav) {
            continue;
        }
        const Dx12Fsr4ReferenceRealizedResourceDesc* resource = findRealizedResource(resources, patch.resolvedResource);
        if (!patch.optional && (resource == nullptr || !resource->validHandle)) {
            ++realization.missingRequiredResourceDescriptorCount;
        }
    }
}

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND

Status hresultStatus(const char* operation, const HRESULT hr)
{
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return Status::runtimeFailure(message.str());
}

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
std::string dx12HresultHex(const HRESULT hr)
{
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<std::uint32_t>(hr);
    return out.str();
}
#endif

D3D12_DESCRIPTOR_RANGE_TYPE dx12RangeType(const Dx12Fsr4ReferenceDescriptorSpace space)
{
    return space == Dx12Fsr4ReferenceDescriptorSpace::Uav ?
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
}

D3D12_FILTER dx12SamplerFilter(const char* filter)
{
    if (filter != nullptr && std::string(filter) == "point") {
        return D3D12_FILTER_MIN_MAG_MIP_POINT;
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D12_TEXTURE_ADDRESS_MODE dx12SamplerAddressMode(const char* addressMode)
{
    if (addressMode != nullptr && std::string(addressMode) == "wrap") {
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
    if (addressMode != nullptr && std::string(addressMode) == "border") {
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

Status createRootSignatureObject(
    ID3D12Device* device,
    const Dx12Fsr4ReferenceRootSignaturePlan& plan,
    ComPtr<ID3D12RootSignature>& outRootSignature)
{
    outRootSignature.Reset();
    if (device == nullptr) {
        return Status::invalidArgument("DX12 FSR4 reference root signature requires a non-null ID3D12Device");
    }
    if (plan.rootParameters.empty()) {
        return Status::invalidArgument("DX12 FSR4 reference root signature plan has no root parameters");
    }

    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    ranges.reserve(plan.rootParameters.size());
    std::vector<D3D12_ROOT_PARAMETER> rootParameters(plan.rootParameters.size());

    for (const Dx12Fsr4ReferenceRootParameterDesc& parameter : plan.rootParameters) {
        if (parameter.rootParameterIndex >= rootParameters.size()) {
            return Status::invalidArgument("DX12 FSR4 reference root parameter index is out of range");
        }

        D3D12_ROOT_PARAMETER& root = rootParameters[parameter.rootParameterIndex];
        root = D3D12_ROOT_PARAMETER{};
        root.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        if (parameter.kind == Dx12Fsr4ReferenceRootParameterKind::RootCbv) {
            root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            root.Descriptor.ShaderRegister = parameter.baseShaderRegister;
            root.Descriptor.RegisterSpace = parameter.registerSpace;
            continue;
        }

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = dx12RangeType(parameter.space);
        range.NumDescriptors = parameter.descriptorCount;
        range.BaseShaderRegister = parameter.baseShaderRegister;
        range.RegisterSpace = parameter.registerSpace;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges.push_back(range);

        root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root.DescriptorTable.NumDescriptorRanges = 1;
        root.DescriptorTable.pDescriptorRanges = &ranges.back();
    }

    // ranges is fully populated now; make descriptor-table pointers stable after
    // any vector reallocation that occurred during construction.
    std::size_t rangeIndex = 0;
    for (D3D12_ROOT_PARAMETER& root : rootParameters) {
        if (root.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            root.DescriptorTable.pDescriptorRanges = &ranges[rangeIndex++];
        }
    }

    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    staticSamplers.reserve(plan.staticSamplers.size());
    for (const Dx12Fsr4ReferenceStaticSamplerDesc& sampler : plan.staticSamplers) {
        D3D12_STATIC_SAMPLER_DESC desc{};
        desc.Filter = dx12SamplerFilter(sampler.filter);
        desc.AddressU = dx12SamplerAddressMode(sampler.addressMode);
        desc.AddressV = dx12SamplerAddressMode(sampler.addressMode);
        desc.AddressW = dx12SamplerAddressMode(sampler.addressMode);
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 16;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        desc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D12_FLOAT32_MAX;
        desc.ShaderRegister = sampler.shaderRegister;
        desc.RegisterSpace = sampler.registerSpace;
        desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        staticSamplers.push_back(desc);
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(rootParameters.size());
    desc.pParameters = rootParameters.data();
    desc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    desc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) {
        return hresultStatus("D3D12SerializeRootSignature", hr);
    }
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&outRootSignature));
    if (FAILED(hr)) {
        return hresultStatus("ID3D12Device::CreateRootSignature", hr);
    }
    return Status::ok();
}

Status createComputePipelineObject(
    ID3D12Device* device,
    ID3D12RootSignature* rootSignature,
    const fsr4_reference_dx12_shader_blobs::ShaderBlob& blob,
    ComPtr<ID3D12PipelineState>& outPipelineState)
{
    outPipelineState.Reset();
    if (device == nullptr || rootSignature == nullptr) {
        return Status::invalidArgument("DX12 FSR4 reference PSO creation requires a device and root signature");
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSignature;
    desc.CS.pShaderBytecode = blob.data;
    desc.CS.BytecodeLength = blob.size;
    desc.NodeMask = 0;
    desc.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    const HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&outPipelineState));
    if (FAILED(hr)) {
        std::ostringstream message;
        message << "ID3D12Device::CreateComputePipelineState(FSR4 reference " << blob.dispatchName << ")";
        return hresultStatus(message.str().c_str(), hr);
    }
    return Status::ok();
}


DXGI_FORMAT dx12ResourceFormat(const SurfaceFormat format)
{
    switch (format) {
    case SurfaceFormat::Rgba32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case SurfaceFormat::Rgba32Uint: return DXGI_FORMAT_R32G32B32A32_UINT;
    case SurfaceFormat::Rgba16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case SurfaceFormat::Rgb32Float: return DXGI_FORMAT_R32G32B32_FLOAT;
    case SurfaceFormat::Rg32Float: return DXGI_FORMAT_R32G32_FLOAT;
    case SurfaceFormat::Rg32Uint: return DXGI_FORMAT_R32G32_UINT;
    case SurfaceFormat::Rg16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case SurfaceFormat::Rg16Uint: return DXGI_FORMAT_R16G16_UINT;
    case SurfaceFormat::Rg16Sint: return DXGI_FORMAT_R16G16_SINT;
    case SurfaceFormat::Rg8Unorm: return DXGI_FORMAT_R8G8_UNORM;
    case SurfaceFormat::Rg8Uint: return DXGI_FORMAT_R8G8_UINT;
    case SurfaceFormat::Rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case SurfaceFormat::Rgba8Snorm: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case SurfaceFormat::Rgba8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case SurfaceFormat::Bgra8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case SurfaceFormat::Bgra8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case SurfaceFormat::R11G11B10Float: return DXGI_FORMAT_R11G11B10_FLOAT;
    case SurfaceFormat::R10G10B10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case SurfaceFormat::R16Float: return DXGI_FORMAT_R16_FLOAT;
    case SurfaceFormat::R16Uint: return DXGI_FORMAT_R16_UINT;
    case SurfaceFormat::R16Unorm: return DXGI_FORMAT_R16_UNORM;
    case SurfaceFormat::R16Snorm: return DXGI_FORMAT_R16_SNORM;
    case SurfaceFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
    case SurfaceFormat::R32Uint: return DXGI_FORMAT_R32_UINT;
    case SurfaceFormat::R8Unorm: return DXGI_FORMAT_R8_UNORM;
    case SurfaceFormat::R8Uint: return DXGI_FORMAT_R8_UINT;
    case SurfaceFormat::R8Sint: return DXGI_FORMAT_R8_SINT;
    case SurfaceFormat::R8Snorm: return DXGI_FORMAT_R8_SNORM;
    case SurfaceFormat::Unknown: return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D12_RESOURCE_FLAGS dx12ResourceFlags(const Dx12Fsr4ReferenceRealizedResourceDesc& resource)
{
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (resource.uav) {
        flags = static_cast<D3D12_RESOURCE_FLAGS>(flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    if (resource.renderTargetCompatible) {
        flags = static_cast<D3D12_RESOURCE_FLAGS>(flags | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    }
    return flags;
}

D3D12_RESOURCE_DESC dx12ResourceDesc(const Dx12Fsr4ReferenceRealizedResourceDesc& resource)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Alignment = 0;
    desc.MipLevels = 1;
    desc.Format = resource.kind == ResourceKind::Buffer ? DXGI_FORMAT_UNKNOWN : dx12ResourceFormat(resource.format);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = resource.kind == ResourceKind::Buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = dx12ResourceFlags(resource);
    if (resource.kind == ResourceKind::Buffer) {
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = resource.byteSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
    } else {
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = resource.width;
        desc.Height = resource.height;
        desc.DepthOrArraySize = 1;
    }
    return desc;
}

std::wstring widenAscii(const char* text)
{
    std::wstring wide;
    if (text == nullptr) {
        return wide;
    }
    while (*text != '\0') {
        wide.push_back(static_cast<unsigned char>(*text++));
    }
    return wide;
}

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
std::string dx12GpuProfileResourceName(const Dx12Fsr4ReferenceRealizedResourceDesc& resource);
void dx12SetGpuProfileObjectName(ID3D12Object* object, const std::string& name) noexcept;
#endif

D3D12_RESOURCE_STATES initialDx12StateForOwnedResource(const Dx12Fsr4ReferenceRealizedResourceDesc& resource) noexcept
{
    return resource.ownership == Dx12Fsr4ReferenceResourceOwnership::UploadOwned ?
        D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
}

Status createOwnedDx12Resource(
    ID3D12Device* device,
    const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
    ComPtr<ID3D12Resource>& outResource)
{
    outResource.Reset();
    if (device == nullptr) {
        return Status::invalidArgument("DX12 FSR4 reference resource creation requires a device");
    }
    if (resource.ownership == Dx12Fsr4ReferenceResourceOwnership::Alias ||
        resource.ownership == Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed) {
        return Status::ok();
    }
    if (resource.kind == ResourceKind::Texture2D && (resource.width == 0 || resource.height == 0)) {
        return Status::invalidArgument("DX12 FSR4 reference texture resource has zero extent");
    }
    if (resource.kind == ResourceKind::Buffer && resource.byteSize == 0) {
        return Status::invalidArgument("DX12 FSR4 reference buffer resource has zero byte size");
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = resource.ownership == Dx12Fsr4ReferenceResourceOwnership::UploadOwned ?
        D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    const D3D12_RESOURCE_DESC desc = dx12ResourceDesc(resource);
    const D3D12_RESOURCE_STATES initialState = initialDx12StateForOwnedResource(resource);
    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (FAILED(hr)) {
        std::ostringstream message;
        message << "ID3D12Device::CreateCommittedResource(FSR4 reference "
                << (resource.name != nullptr ? resource.name : resourceIdName(resource.id)) << ")";
        return hresultStatus(message.str().c_str(), hr);
    }
    const std::wstring debugName = widenAscii(resource.name != nullptr ? resource.name : resourceIdName(resource.id));
    if (!debugName.empty()) {
        outResource->SetName(debugName.c_str());
    }
#if APUSR_ENABLE_GPU_DEBUG_MARKERS
    dx12SetGpuProfileObjectName(outResource.Get(), dx12GpuProfileResourceName(resource));
#endif
    return Status::ok();
}

Status createCbvSrvUavDescriptorHeap(
    ID3D12Device* device,
    const std::uint32_t descriptorCount,
    const bool shaderVisible,
    const wchar_t* debugName,
    ComPtr<ID3D12DescriptorHeap>& outHeap)
{
    outHeap.Reset();
    if (device == nullptr) {
        return Status::invalidArgument("DX12 FSR4 reference descriptor heap creation requires a device");
    }
    if (descriptorCount == 0) {
        return Status::ok();
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = descriptorCount;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&outHeap));
    if (FAILED(hr)) {
        return hresultStatus("ID3D12Device::CreateDescriptorHeap(FSR4 reference CBV/SRV/UAV)", hr);
    }
    if (debugName != nullptr) {
        outHeap->SetName(debugName);
    }
    return Status::ok();
}

Status createShaderVisibleDescriptorHeap(
    ID3D12Device* device,
    const std::uint32_t descriptorCount,
    ComPtr<ID3D12DescriptorHeap>& outHeap)
{
    return createCbvSrvUavDescriptorHeap(
        device,
        descriptorCount,
        true,
        L"APUSR FSR4 reference CBV/SRV/UAV heap",
        outHeap);
}

Status createCpuOnlyDescriptorHeap(
    ID3D12Device* device,
    const std::uint32_t descriptorCount,
    ComPtr<ID3D12DescriptorHeap>& outHeap)
{
    return createCbvSrvUavDescriptorHeap(
        device,
        descriptorCount,
        false,
        L"APUSR FSR4 CPU/DP4A tuned descriptor template heap",
        outHeap);
}

Status createUploadBufferResource(
    ID3D12Device* device,
    const std::uint64_t byteSize,
    const char* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    outResource.Reset();
    if (byteSize == 0) {
        return Status::ok();
    }
    Dx12Fsr4ReferenceRealizedResourceDesc upload{};
    upload.id = ResourceId::Null;
    upload.name = debugName;
    upload.kind = ResourceKind::Buffer;
    upload.ownership = Dx12Fsr4ReferenceResourceOwnership::UploadOwned;
    upload.byteSize = byteSize;
    return createOwnedDx12Resource(device, upload, outResource);
}

D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandleAt(
    ID3D12DescriptorHeap* heap,
    const std::uint32_t descriptorSize,
    const std::uint32_t offset)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorSize) * offset;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandleAt(
    ID3D12DescriptorHeap* heap,
    const std::uint32_t descriptorSize,
    const std::uint32_t offset)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(descriptorSize) * offset;
    return handle;
}

constexpr std::uint32_t kFfxEquivalentDescriptorRingBudget = 4u * 50u * 512u;

std::uint32_t descriptorRingSlotCountForLayout(const std::uint32_t descriptorsPerFrame) noexcept
{
    if (descriptorsPerFrame == 0) {
        return 0;
    }
    const std::uint32_t slots = kFfxEquivalentDescriptorRingBudget / descriptorsPerFrame;
    return std::max<std::uint32_t>(4u, slots);
}

std::uint32_t descriptorFrameBaseOffset(
    const std::uint32_t descriptorsPerFrame,
    const std::uint32_t ringSlot) noexcept
{
    return ringSlot * descriptorsPerFrame;
}

D3D12_SHADER_RESOURCE_VIEW_DESC srvDescForResource(const Dx12Fsr4ReferenceRealizedResourceDesc& resource)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (resource.kind == ResourceKind::Buffer) {
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.NumElements = static_cast<UINT>(resource.byteSize / 4u);
        desc.Buffer.StructureByteStride = 0;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        return desc;
    }
    desc.Format = dx12ResourceFormat(resource.format);
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.MipLevels = 1;
    desc.Texture2D.PlaneSlice = 0;
    desc.Texture2D.ResourceMinLODClamp = 0.0f;
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC uavDescForResource(const Dx12Fsr4ReferenceRealizedResourceDesc& resource)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    if (resource.kind == ResourceKind::Buffer) {
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.NumElements = static_cast<UINT>(resource.byteSize / 4u);
        desc.Buffer.StructureByteStride = 0;
        desc.Buffer.CounterOffsetInBytes = 0;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        return desc;
    }
    desc.Format = dx12ResourceFormat(resource.format);
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.PlaneSlice = 0;
    return desc;
}

DXGI_FORMAT srvFormatFromNativeResourceFormat(const DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    default:
        return format;
    }
}

DXGI_FORMAT uavFormatFromNativeResourceFormat(const DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return srvFormatFromNativeResourceFormat(format);
    }
}

DXGI_FORMAT nativeViewBaseFormat(
    ID3D12Resource* nativeResource,
    const SurfaceFormat frontendFormat,
    const SurfaceFormat fallbackFormat)
{
    const DXGI_FORMAT frontendDxgi = dx12ResourceFormat(frontendFormat);
    if (nativeResource == nullptr) {
        return frontendDxgi != DXGI_FORMAT_UNKNOWN ? frontendDxgi : dx12ResourceFormat(fallbackFormat);
    }
    const DXGI_FORMAT nativeFormat = nativeResource->GetDesc().Format;
    if (nativeFormat == DXGI_FORMAT_UNKNOWN) {
        return frontendDxgi != DXGI_FORMAT_UNKNOWN ? frontendDxgi : dx12ResourceFormat(fallbackFormat);
    }
    // A concrete native resource format is authoritative.  In particular, do
    // not reinterpret an R8G8B8A8 game-owned texture as the frontend's default
    // R16G16B16A16 format. Create*View is void-returning; an incompatible view
    // can poison the D3D12 device and only surface later as DEVICE_REMOVED.
    // Typeless/depth families are normalized by srv/uavFormatFromNative...
    // below, so the frontend format is only a fallback for format-less inputs.
    return nativeFormat;
}

bool resourceFormatSupportsSrv(ID3D12Resource* resource, const DXGI_FORMAT view) noexcept
{
    if (resource == nullptr || view == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = view;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
        return false;
    }
    return (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
}

bool resourceFormatSupportsUav(ID3D12Resource* resource, const DXGI_FORMAT view) noexcept
{
    if (resource == nullptr || view == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = view;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
        return false;
    }
    return (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

SurfaceFormat frontendFormatForExternalResource(
    const Dx12Fsr4ReferenceFrameDesc& frameDesc,
    const ResourceId id) noexcept
{
    switch (id) {
    case ResourceId::InputColor:
        return frameDesc.externalResources.inputColorFormat;
    case ResourceId::InputDepth:
        return frameDesc.externalResources.inputDepthFormat;
    case ResourceId::InputMotionVectors:
        return frameDesc.externalResources.inputMotionVectorsFormat;
    case ResourceId::InputExposure:
        return frameDesc.externalResources.inputExposureFormat;
    case ResourceId::FinalOutput:
        return frameDesc.externalResources.outputFormat;
    default:
        return SurfaceFormat::Unknown;
    }
}

DXGI_FORMAT srvViewFormatForResource(
    const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
    ID3D12Resource* nativeResource,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    if (resource.ownership == Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed) {
        const DXGI_FORMAT base = nativeViewBaseFormat(
            nativeResource,
            frontendFormatForExternalResource(frameDesc, resource.id),
            resource.format);
        return srvFormatFromNativeResourceFormat(base);
    }
    return dx12ResourceFormat(resource.format);
}

DXGI_FORMAT uavViewFormatForResource(
    const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
    ID3D12Resource* nativeResource,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    if (resource.ownership == Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed) {
        const DXGI_FORMAT base = nativeViewBaseFormat(
            nativeResource,
            frontendFormatForExternalResource(frameDesc, resource.id),
            resource.format);
        return uavFormatFromNativeResourceFormat(base);
    }
    return dx12ResourceFormat(resource.format);
}

std::string nativeResourceDescSummary(ID3D12Resource* resource)
{
    if (resource == nullptr) {
        return "resource=null";
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    std::ostringstream out;
    out << "resourceDesc{dim=" << static_cast<unsigned>(desc.Dimension)
        << ", format=" << static_cast<unsigned>(desc.Format)
        << ", width=" << static_cast<unsigned long long>(desc.Width)
        << ", height=" << desc.Height
        << ", flags=0x" << std::hex << static_cast<unsigned>(desc.Flags) << std::dec << '}';
    return out.str();
}

[[maybe_unused]] Status validateNativeDescriptorView(
    const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
    ID3D12Resource* nativeResource,
    const Dx12Fsr4ReferenceDescriptorSpace space,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    if (nativeResource == nullptr) {
        return Status::invalidArgument("DX12 FSR4 reference descriptor validation requires a non-null native resource");
    }
    const D3D12_RESOURCE_DESC nativeDesc = nativeResource->GetDesc();
    if (resource.kind == ResourceKind::Buffer) {
        if (nativeDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            std::ostringstream message;
            message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                    << " expected a buffer but got " << nativeResourceDescSummary(nativeResource);
            return Status::invalidArgument(message.str());
        }
        if ((nativeDesc.Width % 4u) != 0u) {
            std::ostringstream message;
            message << "DX12 FSR4 reference buffer resource " << resourceIdName(resource.id)
                    << " is not 32-bit aligned: " << nativeResourceDescSummary(nativeResource);
            return Status::invalidArgument(message.str());
        }
        if (space == Dx12Fsr4ReferenceDescriptorSpace::Uav &&
            (nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
            std::ostringstream message;
            message << "DX12 FSR4 reference buffer resource " << resourceIdName(resource.id)
                    << " does not allow UAV creation: " << nativeResourceDescSummary(nativeResource);
            return Status::invalidArgument(message.str());
        }
        // Raw buffer SRV/UAV descriptors use DXGI_FORMAT_R32_TYPELESS plus RAW flags.
        // They should not be validated through typed texture load/store format support.
        return Status::ok();
    }
    if (resource.kind == ResourceKind::Texture2D && nativeDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        std::ostringstream message;
        message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                << " expected a 2D texture but got " << nativeResourceDescSummary(nativeResource);
        return Status::invalidArgument(message.str());
    }
    if (resource.kind == ResourceKind::Texture2D && nativeDesc.SampleDesc.Count != 1u) {
        std::ostringstream message;
        message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                << " is multisampled but the FSR4 descriptor contract requires Texture2D: "
                << nativeResourceDescSummary(nativeResource);
        return Status::invalidArgument(message.str());
    }
    if (resource.kind == ResourceKind::Texture2D && nativeDesc.DepthOrArraySize != 1u) {
        std::ostringstream message;
        message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                << " is an array/3D-compatible texture but the FSR4 descriptor contract requires one 2D slice: "
                << nativeResourceDescSummary(nativeResource);
        return Status::invalidArgument(message.str());
    }
    if (space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
        const DXGI_FORMAT view = srvViewFormatForResource(resource, nativeResource, frameDesc);
        if (resource.kind == ResourceKind::Texture2D && (nativeDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
            std::ostringstream message;
            message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                    << " denies SRV creation: " << nativeResourceDescSummary(nativeResource);
            return Status::invalidArgument(message.str());
        }
        if (view == DXGI_FORMAT_UNKNOWN || !resourceFormatSupportsSrv(nativeResource, view)) {
            std::ostringstream message;
            message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                    << " has incompatible SRV view format=" << static_cast<unsigned>(view)
                    << " frontendFormat=" << static_cast<unsigned>(frontendFormatForExternalResource(frameDesc, resource.id))
                    << ' ' << nativeResourceDescSummary(nativeResource);
            return Status::invalidArgument(message.str());
        }
        return Status::ok();
    }
    if (space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
        const DXGI_FORMAT view = uavViewFormatForResource(resource, nativeResource, frameDesc);
        if (resource.kind == ResourceKind::Texture2D && (nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
            std::ostringstream message;
            message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                    << " does not allow UAV creation: " << nativeResourceDescSummary(nativeResource);
            return Status::invalidArgument(message.str());
        }
        if (view == DXGI_FORMAT_UNKNOWN || !resourceFormatSupportsUav(nativeResource, view)) {
            std::ostringstream message;
            message << "DX12 FSR4 reference resource " << resourceIdName(resource.id)
                    << " has incompatible UAV view format=" << static_cast<unsigned>(view)
                    << " frontendFormat=" << static_cast<unsigned>(frontendFormatForExternalResource(frameDesc, resource.id))
                    << ' ' << nativeResourceDescSummary(nativeResource);
            return Status::invalidArgument(message.str());
        }
        return Status::ok();
    }
    return Status::ok();
}

D3D12_SHADER_RESOURCE_VIEW_DESC srvDescForResource(
    const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
    ID3D12Resource* nativeResource,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = srvDescForResource(resource);
    if (resource.kind != ResourceKind::Buffer) {
        desc.Format = srvViewFormatForResource(resource, nativeResource, frameDesc);
    }
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC uavDescForResource(
    const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
    ID3D12Resource* nativeResource,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = uavDescForResource(resource);
    if (resource.kind != ResourceKind::Buffer) {
        desc.Format = uavViewFormatForResource(resource, nativeResource, frameDesc);
    }
    return desc;
}

void initializeNullDescriptors(
    ID3D12Device* device,
    ID3D12DescriptorHeap* heap,
    const std::uint32_t descriptorSize,
    const Dx12Fsr4ReferenceRootSignaturePlan& rootSignature,
    const std::uint32_t descriptorBaseOffset)
{
    if (device == nullptr || heap == nullptr) {
        return;
    }
    Dx12Fsr4ReferenceRealizedResourceDesc nullSrv{};
    nullSrv.kind = ResourceKind::Texture2D;
    nullSrv.format = SurfaceFormat::R32Float;
    nullSrv.width = 1;
    nullSrv.height = 1;
    const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = srvDescForResource(nullSrv);
    for (std::uint32_t i = 0; i < rootSignature.descriptorHeap.srvCount; ++i) {
        device->CreateShaderResourceView(nullptr, &srvDesc, cpuDescriptorHandleAt(heap, descriptorSize, descriptorBaseOffset + rootSignature.descriptorHeap.srvBaseOffset + i));
    }

    Dx12Fsr4ReferenceRealizedResourceDesc nullUav{};
    nullUav.kind = ResourceKind::Texture2D;
    nullUav.format = SurfaceFormat::R32Float;
    nullUav.width = 1;
    nullUav.height = 1;
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = uavDescForResource(nullUav);
    for (std::uint32_t i = 0; i < rootSignature.descriptorHeap.uavCount; ++i) {
        device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, cpuDescriptorHandleAt(heap, descriptorSize, descriptorBaseOffset + rootSignature.descriptorHeap.uavBaseOffset + i));
    }
}

Status writeUploadBufferBytes(
    ID3D12Resource* uploadResource,
    const std::uint64_t byteOffset,
    const void* bytes,
    const std::uint64_t byteSize)
{
    if (byteSize == 0) {
        return Status::ok();
    }
    if (uploadResource == nullptr || bytes == nullptr) {
        return Status::invalidArgument("DX12 FSR4 reference upload write requires non-null buffer and source bytes");
    }
    void* mapped = nullptr;
    D3D12_RANGE readRange{};
    const HRESULT hr = uploadResource->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        return hresultStatus("ID3D12Resource::Map(FSR4 reference upload)", hr);
    }
    std::memcpy(static_cast<std::uint8_t*>(mapped) + byteOffset, bytes, static_cast<std::size_t>(byteSize));
    D3D12_RANGE writtenRange{};
    writtenRange.Begin = static_cast<SIZE_T>(byteOffset);
    writtenRange.End = static_cast<SIZE_T>(byteOffset + byteSize);
    uploadResource->Unmap(0, &writtenRange);
    return Status::ok();
}

D3D12_RESOURCE_STATES normalizeDx12ResourceState(const std::uint64_t state) noexcept
{
    if (state == 0ull ||
        state == kDx12Fsr4ReferenceUnknownNativeResourceState ||
        state == 0xFFFFFFFFull) {
        return D3D12_RESOURCE_STATE_COMMON;
    }
    return static_cast<D3D12_RESOURCE_STATES>(state);
}

std::uint64_t externalNativeStateForResource(
    const Dx12Fsr4ReferenceFrameDesc& frameDesc,
    const ResourceId resource) noexcept
{
    switch (resource) {
    case ResourceId::InputColor:
        return frameDesc.externalResources.inputColorState;
    case ResourceId::InputDepth:
        return frameDesc.externalResources.inputDepthState;
    case ResourceId::InputMotionVectors:
        return frameDesc.externalResources.inputMotionVectorsState;
    case ResourceId::InputExposure:
        return frameDesc.externalResources.inputExposureState;
    case ResourceId::FinalOutput:
        return frameDesc.externalResources.outputState;
    default:
        return kDx12Fsr4ReferenceUnknownNativeResourceState;
    }
}

D3D12_RESOURCE_STATES desiredStateForDescriptorSpace(const Dx12Fsr4ReferenceDescriptorSpace space) noexcept
{
    return space == Dx12Fsr4ReferenceDescriptorSpace::Uav ?
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

bool descriptorPatchWritesResource(const Dx12Fsr4ReferenceDescriptorPatchDesc& patch) noexcept
{
    return patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav;
}


bool dx12GpuTimestampEnvTruth(const char* name)
{
    if (name == nullptr) {
        return false;
    }
#if defined(_WIN32)
    char buffer[32]{};
    const DWORD size = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (size == 0 || size >= sizeof(buffer)) {
        return false;
    }
    const char c = buffer[0];
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const char c = value[0];
#endif
    return c != '0' && c != 'n' && c != 'N' && c != 'f' && c != 'F' && c != 'o' && c != 'O';
}

std::string dx12GpuTimestampEnvString(const char* name)
{
    if (name == nullptr) {
        return {};
    }
#if defined(_WIN32)
    DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string value(needed, '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

std::uint32_t dx12GpuTimestampEnvU32(const char* name, const std::uint32_t fallback)
{
    const std::string value = dx12GpuTimestampEnvString(name);
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    return end != value.c_str() ? static_cast<std::uint32_t>(parsed) : fallback;
}

std::string dx12GpuTimestampDefaultTraceDirectory()
{
    std::string explicitDir = dx12GpuTimestampEnvString("APUSR_LOG_DIR");
    if (!explicitDir.empty()) {
        return explicitDir;
    }
#if defined(_WIN32)
    return "C:/NGXLogs/APUSR";
#else
    return "/tmp/APUSR";
#endif
}

std::string dx12GpuTimestampDefaultTracePath()
{
    std::string explicitPath = dx12GpuTimestampEnvString("APUSR_DX12_GPU_TIMESTAMPS_FILE");
    if (!explicitPath.empty()) {
        return explicitPath;
    }
    return dx12GpuTimestampDefaultTraceDirectory() + "/apusr_dx12_gpu_timestamps.csv";
}

bool dx12LegacyGpuTimestampsEnabled() noexcept
{
    static const bool enabled = dx12GpuTimestampEnvTruth("APUSR_DX12_GPU_TIMESTAMPS");
    return enabled;
}

bool dx12GpuTimestampsEnabled() noexcept
{
    return dx12LegacyGpuTimestampsEnabled() || apusr::profile::gpuEnabled();
}

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
bool dx12GpuDebugMarkersEnabled() noexcept
{
    static const bool enabled = dx12GpuTimestampEnvTruth("APUSR_DX12_GPU_MARKERS");
    return enabled;
}

bool dx12GpuResourceNamesEnabled() noexcept
{
    static const bool enabled = dx12GpuDebugMarkersEnabled() || dx12GpuTimestampEnvTruth("APUSR_DX12_RESOURCE_NAMES");
    return enabled;
}

enum class Dx12GpuAnnotationModeStatus {
    NotRequested,
    Configured,
    Unsupported,
    HeaderUnavailable,
    QueryInterfaceFailed,
    FeatureCheckFailed,
    SetModeFailed,
};

const char* dx12GpuAnnotationModeStatusName(const Dx12GpuAnnotationModeStatus status) noexcept
{
    switch (status) {
    case Dx12GpuAnnotationModeStatus::NotRequested: return "not_requested";
    case Dx12GpuAnnotationModeStatus::Configured: return "configured";
    case Dx12GpuAnnotationModeStatus::Unsupported: return "unsupported";
    case Dx12GpuAnnotationModeStatus::HeaderUnavailable: return "header_unavailable";
    case Dx12GpuAnnotationModeStatus::QueryInterfaceFailed: return "query_interface_failed";
    case Dx12GpuAnnotationModeStatus::FeatureCheckFailed: return "feature_check_failed";
    case Dx12GpuAnnotationModeStatus::SetModeFailed: return "set_mode_failed";
    }
    return "unknown";
}

class Dx12GpuDebugEventScope final {
public:
    Dx12GpuDebugEventScope(ID3D12GraphicsCommandList* commandList, const char* name) noexcept
        : commandList_(commandList)
    {
        if (!dx12GpuDebugMarkersEnabled() || commandList_ == nullptr || name == nullptr || name[0] == '\0') {
            return;
        }
        PIXBeginEvent(commandList_, 0u, "%s", name);
        pixActive_ = true;
    }

    Dx12GpuDebugEventScope(const Dx12GpuDebugEventScope&) = delete;
    Dx12GpuDebugEventScope& operator=(const Dx12GpuDebugEventScope&) = delete;

    ~Dx12GpuDebugEventScope() noexcept
    {
        if (pixActive_ && commandList_ != nullptr) {
            PIXEndEvent(commandList_);
        }
    }

private:
    ID3D12GraphicsCommandList* commandList_ = nullptr;
    bool pixActive_ = false;
};

std::string dx12GpuProfileResourceName(const Dx12Fsr4ReferenceRealizedResourceDesc& resource)
{
    std::ostringstream name;
    name << "APUSR.FSR4.";
    switch (resource.ownership) {
    case Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed: name << "external."; break;
    case Dx12Fsr4ReferenceResourceOwnership::InternalOwned: name << "owned."; break;
    case Dx12Fsr4ReferenceResourceOwnership::UploadOwned: name << "upload."; break;
    case Dx12Fsr4ReferenceResourceOwnership::Alias: name << "alias."; break;
    }
    name << (resource.name != nullptr ? resource.name : resourceIdName(resource.id));
    name << "." << resourceIdName(resource.id);
    return name.str();
}

void dx12SetGpuProfileObjectName(ID3D12Object* object, const std::string& name) noexcept
{
    if (object == nullptr || !dx12GpuResourceNamesEnabled() || name.empty()) {
        return;
    }
    const std::wstring wideName = widenAscii(name.c_str());
    if (!wideName.empty()) {
        object->SetName(wideName.c_str());
    }
}
#endif

struct Dx12GpuTimestampDispatchMetadata {
    std::string name;
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
    std::uint32_t groupsZ = 0;
    std::uint32_t profileLevel = 0xffffffffu;
    std::uint32_t queryBeginOffset = 0;
    std::uint32_t queryDispatchBeginOffset = 0;
    std::uint32_t queryEndOffset = 0;
    std::uint32_t srvBindingCount = 0;
    std::uint32_t uavBindingCount = 0;
    std::uint64_t srvUniqueBytes = 0;
    std::uint64_t uavUniqueBytes = 0;
    std::uint64_t srvExternalBytes = 0;
    std::uint64_t uavExternalBytes = 0;
    std::uint64_t srvTemporalBytes = 0;
    std::uint64_t uavTemporalBytes = 0;
    std::uint64_t scratchSrvBytes = 0;
    std::uint64_t scratchUavBytes = 0;
};

struct Dx12GpuTimestampFrameMetadata {
    bool valid = false;
    bool logged = false;
    bool unifiedProfile = false;
    bool configurationChanged = false;
    std::uint64_t cpuFrameSequence = 0;
    std::uint64_t modelFrameIndex = 0;
    std::uint64_t sampleSequence = 0;
    std::uint64_t burstSequence = 0;
    std::uint64_t configurationHash = 0;
    std::uint32_t burstFrame = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    std::uint32_t slot = 0;
    double cpuSecondsSinceStart = 0.0;
    std::uint32_t resolvedQueryCount = 0;
    std::uint32_t frameBeginQueryOffset = 0;
    std::uint32_t frameEndQueryOffset = 0;
    std::uint64_t timestampFrequency = 0;
    std::uint64_t nativeQueue = 0;
    std::uint64_t cpuTotalNs = 0;
    std::uint64_t cpuRecordNs = 0;
    std::uint32_t dispatchCount = 0;
    std::vector<Dx12GpuTimestampDispatchMetadata> dispatches;
};


#endif

Status validateDx12Fsr4ReferencePlanInputs(
    const Dx12Fsr4ReferenceCreateDesc& createDesc,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    if (createDesc.runner.maxRenderSize.width == 0 || createDesc.runner.maxRenderSize.height == 0 ||
        createDesc.runner.maxUpscaleSize.width == 0 || createDesc.runner.maxUpscaleSize.height == 0) {
        return Status::invalidArgument("DX12 FSR4 reference backend requires non-zero maximum render and output sizes");
    }
    if (frameDesc.runner.frame.renderSize.width == 0 || frameDesc.runner.frame.renderSize.height == 0 ||
        frameDesc.runner.frame.displaySize.width == 0 || frameDesc.runner.frame.displaySize.height == 0) {
        return Status::invalidArgument("DX12 FSR4 reference frame requires non-zero render and display sizes");
    }
    if (!isReferenceDisplaySizeSupported(createDesc.runner.maxUpscaleSize)) {
        return Status::invalidArgument("DX12 FSR4 reference backend supports maximum output sizes up to 7680x4320");
    }
    if (!isReferenceDisplaySizeSupported(frameDesc.runner.frame.displaySize)) {
        return Status::invalidArgument("DX12 FSR4 reference frame output size exceeds 7680x4320");
    }
    return Status::ok();
}

struct Dx12RuntimePlanStructureKey {
    PipelineVariant pipelineVariant = PipelineVariant::StrictReference;
    Preset preset = Preset::Balanced;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    std::uint32_t permutationKey = 0;
    std::uint32_t alignedDisplayWidth = 0;
    std::uint32_t alignedDisplayHeight = 0;
    std::uint32_t spdGroupsX = 0;
    std::uint32_t spdGroupsY = 0;
    bool sharpeningRequested = false;
    bool useDefaultExposure = false;

    bool operator==(const Dx12RuntimePlanStructureKey&) const = default;
};

Dx12RuntimePlanStructureKey makeDx12RuntimePlanStructureKey(
    const Dx12Fsr4ReferenceCreateDesc& createDesc,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    ReferenceRunnerFrameDesc effectiveFrame = frameDesc.runner;
    if (effectiveFrame.frame.displaySize.width == 0 && effectiveFrame.frame.displaySize.height == 0) {
        effectiveFrame.frame.displaySize = createDesc.runner.maxUpscaleSize;
    }
    const Preset preset = referencePresetForFrame(effectiveFrame);
    const FrameCapacity frameCapacity = frameCapacityForDisplay(
        createDesc.runner.maxUpscaleSize.width != 0 ? createDesc.runner.maxUpscaleSize :
        alignReferenceUpscaleSize(effectiveFrame.frame.displaySize));
    const PermutationDesc permutation = makeReferencePermutationDesc(
        createDesc.runner, preset, frameCapacity, effectiveFrame.frame.options);
    const DisplaySize alignedDisplay = alignReferenceUpscaleSize(effectiveFrame.frame.displaySize);
    const auto groups64 = [](const std::uint32_t value) noexcept {
        return (value + 63u) / 64u;
    };
    return Dx12RuntimePlanStructureKey{
        .pipelineVariant = createDesc.runner.pipelineVariant,
        .preset = preset,
        .frameCapacity = frameCapacity,
        .permutationKey = apusr::models::fsr4_int8::permutationKey(permutation),
        .alignedDisplayWidth = alignedDisplay.width,
        .alignedDisplayHeight = alignedDisplay.height,
        .spdGroupsX = createDesc.runner.features.autoExposure ? groups64(effectiveFrame.frame.renderSize.width) : 0u,
        .spdGroupsY = createDesc.runner.features.autoExposure ? groups64(effectiveFrame.frame.renderSize.height) : 0u,
        .sharpeningRequested = effectiveFrame.frame.options.sharpeningRequested,
        .useDefaultExposure = !effectiveFrame.frame.options.hasExposure,
    };
}

[[maybe_unused]] std::uint64_t descriptorTemplateCacheKeyForPlan(const ReferenceFramePlan& framePlan)
{
    std::uint64_t key = reflectionValidationCacheKeyForPlan(framePlan);
    key = apusr::profile::hashCombine(key, framePlan.constants.useDefaultExposure != 0u ? 1u : 0u);
    return key;
}

class PlannedDx12Fsr4ReferenceRuntime final : public Dx12Fsr4ReferenceRuntime {
public:
    ~PlannedDx12Fsr4ReferenceRuntime() override
    {
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        recordOwnedResourceFreeEvents(0u);
        unmapConstantUploadBuffer();
#endif
    }

    Status initialize(const Dx12Fsr4ReferenceCreateDesc& desc) override
    {
        createDesc_ = desc;
        profileStream_ = apusr::profile::registerStream(apusr::profile::StreamDesc{
            .backend = apusr::profile::BackendApi::Dx12,
            .component = apusr::profile::Component::Upscaler,
            .name = "fsr4.reference",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityFrames |
                apusr::profile::StreamCapabilityGpuIntervals |
                apusr::profile::StreamCapabilityCalibration |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityInstants |
                apusr::profile::StreamCapabilityErrors |
                apusr::profile::StreamCapabilityStateTransitions |
                apusr::profile::StreamCapabilityResourceEvents |
                apusr::profile::StreamCapabilityGpuWorkloadInfo,
            .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(desc.device.value)),
        });
        profileSampleGate_.reset();
        reflectionManifestLoaded_ = false;
        reflectionValidationCacheKey_ = std::numeric_limits<std::uint64_t>::max();
        runtimePlanGeneration_ = 0;
        preparedResourceCacheKey_ = std::numeric_limits<std::uint64_t>::max();
        realizedResourceLookupGeneration_ = 0;
        resourceGeneration_ = 0;
        initializerBytes_.clear();
        initializerPresetValid_ = false;
        modelResetKeyValid_ = false;
        modelResetKey_ = 0;
        runtimePlanCacheValid_ = false;
        runtimePlanCache_ = Dx12Fsr4ReferenceBackendPlan{};
        preparedPipelinePlanKey_ = std::numeric_limits<std::uint64_t>::max();
        if (desc.requireCompleteReflectionCoverage && !desc.enableReflectionManifestValidation) {
            return Status::invalidArgument("DX12 FSR4 reference runtime cannot require complete reflection coverage while reflection validation is disabled");
        }
        if (desc.requireCompleteReflectionCoverage && desc.reflectionManifestPath.empty()) {
            return Status::invalidArgument("DX12 FSR4 reference runtime requires complete reflection coverage but no manifest path was provided");
        }
        if (desc.enableReflectionManifestValidation && !desc.reflectionManifestPath.empty()) {
            const Status manifestStatus = loadDx12Fsr4ReferenceShaderReflectionManifest(
                desc.reflectionManifestPath,
                reflectionManifest_);
            if (!manifestStatus.succeeded()) {
                return manifestStatus;
            }
            reflectionManifestLoaded_ = true;
        }
        // Build the long-lived root-signature envelope from the same exact
        // per-dispatch contracts used for frame recording.
        const ReferenceFramePlan rootEnvelopePlan = makeDx12RootSignatureEnvelopeFramePlan(desc.runner);
        const std::vector<Dx12Fsr4ReferenceDescriptorBinding> rootSignatureBindings = makeDx12DescriptorBindings(
            rootEnvelopePlan,
            cachedSrvCount_,
            cachedUavCount_,
            cachedCbvCount_,
            cachedSamplerCount_);
        rootSignaturePlan_ = makeDx12Fsr4ReferenceRootSignaturePlan(
            rootSignatureBindings,
            cachedSrvCount_,
            cachedUavCount_);

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        recordOwnedResourceFreeEvents(0u);
        unmapConstantUploadBuffer();
        device_.Reset();
        profileAdapter_.Reset();
        profileMemoryBudgetUnavailableLogged_ = false;
        rootSignature_.Reset();
        descriptorHeap_.Reset();
        descriptorTemplateHeap_.Reset();
        constantUploadBuffer_.Reset();
        defaultExposureUploadBuffer_.Reset();
        defaultExposureUploadFootprint_ = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
        descriptorSize_ = 0;
        descriptorTemplateDescriptorsPerFrame_ = 0;
        descriptorTemplateResourceGeneration_ = std::numeric_limits<std::uint64_t>::max();
        descriptorTemplatePlanKey_ = std::numeric_limits<std::uint64_t>::max();
        tunedNativeDispatchCacheKey_ = std::numeric_limits<std::uint64_t>::max();
        tunedNativeDispatchCache_.clear();
        resetGpuTimestampProfiler();
        gpuTimestampProfilerDisabledAfterFailure_ = false;
        gpuTimestampCsvHeaderWritten_ = false;
        constantUploadBufferByteSize_ = 0;
        currentConstantUploadBaseOffset_ = 0;
        currentConstantUploadGpuBase_ = 0;
        constantUploadFrameIndex_ = 0;
        pipelineStates_.clear();
        ownedResources_.clear();
        resetFastResourceLookup();
        validatedExternalDescriptorResourceById_.fill(nullptr);
        validatedExternalDescriptorSpacesById_.fill(0u);
        pendingResourceInitialization_ = false;
        if (desc.device.value != nullptr) {
            device_ = static_cast<ID3D12Device*>(desc.device.value);
            const Status rootStatus = createRootSignatureObject(
                device_.Get(),
                rootSignaturePlan_,
                rootSignature_);
            if (!rootStatus.succeeded()) {
                return rootStatus;
            }
        }
#endif

        initialized_ = true;
        return Status::ok();
    }

    Status planFrame(const Dx12Fsr4ReferenceFrameDesc& desc, Dx12Fsr4ReferenceBackendPlan& outPlan) override
    {
        if (!initialized_) {
            return Status::invalidArgument("DX12 FSR4 reference runtime must be initialized before planning a frame");
        }
        Dx12Fsr4ReferenceBackendPlan* runtimePlan = nullptr;
        const Status planStatus = runtimePlanForFrame(desc, runtimePlan);
        if (!planStatus.succeeded()) {
            return planStatus;
        }
        outPlan = *runtimePlan;
        return Status::ok();
    }

    Status prepareFrameCache(const Dx12Fsr4ReferenceFrameDesc& desc) override
    {
        Dx12Fsr4ReferenceBackendPlan* runtimePlan = nullptr;
        const Status status = runtimePlanForFrame(desc, runtimePlan);
        if (!status.succeeded()) {
            return status;
        }
        Dx12Fsr4ReferenceBackendPlan& plan = *runtimePlan;
        const Status resourceStatus = prepareResources(plan, desc);
        if (!resourceStatus.succeeded()) {
            return resourceStatus;
        }
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        {
            const Status health = validateDeviceHealth("prepare_frame_cache.resources");
            if (!health.succeeded()) return health;
        }
#endif
        const Status pipelineStatus = preparePipelines(plan);
        if (!pipelineStatus.succeeded()) {
            return pipelineStatus;
        }
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        {
            const Status health = validateDeviceHealth("prepare_frame_cache.pipelines");
            if (!health.succeeded()) return health;
        }
        const Status tunedNativeStatus = rebuildTunedNativeDispatchCache(plan);
        if (!tunedNativeStatus.succeeded()) {
            return tunedNativeStatus;
        }
        {
            const Status health = validateDeviceHealth("prepare_frame_cache.native_dispatch_cache");
            if (!health.succeeded()) return health;
        }
#endif
        const Status constantStatus = prepareConstantUploads(plan);
        if (!constantStatus.succeeded()) {
            return constantStatus;
        }
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        return validateDeviceHealth("prepare_frame_cache.constants");
#else
        return Status::ok();
#endif
    }

    Status recordFrame(const Dx12Fsr4ReferenceFrameDesc& desc) override
    {
        const std::uint64_t profileFrameId = desc.runner.frame.frameIndex;
        const bool cpuProfilingEnabled = profileStream_ && apusr::profile::cpuEnabled();
        const std::uint64_t cpuFrameBegin = cpuProfilingEnabled ? apusr::profile::cpuTick() : 0u;
        const auto fail = [&](const char* stage, Status failure) -> Status {
            apusr::profile::recordStatus(profileStream_, profileFrameId, stage, failure);
            return failure;
        };

        Dx12Fsr4ReferenceBackendPlan* runtimePlan = nullptr;
        const std::uint64_t planBegin = cpuFrameBegin;
        const Status status = runtimePlanForFrame(desc, runtimePlan);
        const std::uint64_t planEnd = cpuProfilingEnabled ? apusr::profile::cpuTick() : 0u;
        if (!status.succeeded()) {
            return fail("record_frame.plan", status);
        }
        Dx12Fsr4ReferenceBackendPlan& plan = *runtimePlan;

        const std::uint64_t configurationHash = profileConfigurationHash(plan, desc);
        apusr::profile::SampleDecision profileDecision{};
        if (profileStream_ && apusr::profile::enabled()) {
            if (configurationHash != profileLastConfigurationHash_) {
                apusr::profile::recordStateTransition({
                    .stream = profileStream_,
                    .frameId = profileFrameId,
                    .name = "upscaler.configuration_hash",
                    .reason = profileLastConfigurationHash_ == 0u ? "initial" : "configuration_change",
                    .previousValue = profileLastConfigurationHash_,
                    .nextValue = configurationHash,
                });
                profileLastConfigurationHash_ = configurationHash;
            }
            profileDecision = profileSampleGate_.decide(configurationHash, profileFrameId);
        }
        const bool captureCpu = profileDecision.capture && apusr::profile::cpuEnabled();
        const auto recordCpuStage = [&](const char* name, const std::uint64_t begin, const std::uint64_t end) noexcept {
            if (!captureCpu || begin == 0u || end < begin) {
                return;
            }
            apusr::profile::recordCpuInterval({
                .stream = profileStream_,
                .frameId = profileFrameId,
                .sampleSequence = profileDecision.sampleSequence,
                .startCpuTick = begin,
                .endCpuTick = end,
                .name = name,
            });
        };
        recordCpuStage("upscaler.plan", planBegin, planEnd);
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        if (profileDecision.capture) {
            recordVideoMemoryBudget(profileFrameId, profileDecision.burstFrame);
        }
#endif

        const std::uint64_t nextModelResetKey = modelResetKeyForPlan(plan.framePlan);
        const bool forceModelReset = desc.runner.forceReset || !modelResetKeyValid_ || modelResetKey_ != nextModelResetKey;
        if (forceModelReset) {
            plan.framePlan.constants.reset = 1u;
        }

        std::uint64_t stageBegin = captureCpu ? apusr::profile::cpuTick() : 0u;
        const Status resourceStatus = prepareResources(plan, desc);
        std::uint64_t stageEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
        recordCpuStage("upscaler.prepare_resources", stageBegin, stageEnd);
        if (!resourceStatus.succeeded()) {
            return fail("record_frame.prepare_resources", resourceStatus);
        }

        stageBegin = captureCpu ? apusr::profile::cpuTick() : 0u;
        const Status pipelineStatus = preparePipelines(plan);
        stageEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
        recordCpuStage("upscaler.prepare_pipelines", stageBegin, stageEnd);
        if (!pipelineStatus.succeeded()) {
            return fail("record_frame.prepare_pipelines", pipelineStatus);
        }
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        stageBegin = captureCpu ? apusr::profile::cpuTick() : 0u;
        const Status tunedNativeStatus = rebuildTunedNativeDispatchCache(plan);
        stageEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
        recordCpuStage("upscaler.prepare_native_dispatch_cache", stageBegin, stageEnd);
        if (!tunedNativeStatus.succeeded()) {
            return fail("record_frame.native_dispatch_cache", tunedNativeStatus);
        }
#endif

        stageBegin = captureCpu ? apusr::profile::cpuTick() : 0u;
        const Status descriptorStatus = prepareDescriptors(plan, desc);
        stageEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
        recordCpuStage("upscaler.prepare_descriptors", stageBegin, stageEnd);
        if (!descriptorStatus.succeeded()) {
            return fail("record_frame.prepare_descriptors", descriptorStatus);
        }
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        {
            const Status health = validateDeviceHealth("record_frame.prepare_descriptors");
            if (!health.succeeded()) {
                return fail("record_frame.prepare_descriptors.device_health", health);
            }
        }
#endif

        stageBegin = captureCpu ? apusr::profile::cpuTick() : 0u;
        const Status constantStatus = prepareConstantUploads(plan);
        stageEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
        recordCpuStage("upscaler.prepare_constants", stageBegin, stageEnd);
        if (!constantStatus.succeeded()) {
            return fail("record_frame.prepare_constants", constantStatus);
        }

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        if (desc.commandList.value != nullptr) {
            if (device_ == nullptr) {
                return fail("record_frame.native_device", Status::invalidArgument(
                    "DX12 FSR4 reference recording requires a native device when a command list is supplied"));
            }

            std::uint32_t gpuTimestampSlot = std::numeric_limits<std::uint32_t>::max();
            const std::uint64_t recordBegin = captureCpu ? apusr::profile::cpuTick() : 0u;
            const Status recordStatus = recordNativeDispatches(
                plan, desc, profileDecision, configurationHash, cpuFrameBegin, recordBegin, gpuTimestampSlot);
            const std::uint64_t recordEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
            recordCpuStage("upscaler.record_commands", recordBegin, recordEnd);
            if (!recordStatus.succeeded()) {
                return fail("record_frame.record_native", recordStatus);
            }

            modelResetKey_ = nextModelResetKey;
            modelResetKeyValid_ = true;

            if (profileDecision.capture && (!apusr::profile::gpuEnabled() ||
                gpuTimestampSlot == std::numeric_limits<std::uint32_t>::max())) {
                std::uint64_t totalWorkgroups = 0u;
                for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
                    if (dispatch.enabled) {
                        totalWorkgroups += static_cast<std::uint64_t>(dispatch.grid.groupsX) *
                            dispatch.grid.groupsY * dispatch.grid.groupsZ;
                    }
                }
                const std::uint64_t cpuFrameEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
                apusr::profile::recordFrameSample({
                    .stream = profileStream_,
                    .frameId = profileFrameId,
                    .sampleSequence = profileDecision.sampleSequence,
                    .burstSequence = profileDecision.burstSequence,
                    .configurationHash = configurationHash,
                    .burstFrame = profileDecision.burstFrame,
                    .renderWidth = desc.runner.frame.renderSize.width,
                    .renderHeight = desc.runner.frame.renderSize.height,
                    .displayWidth = desc.runner.frame.displaySize.width,
                    .displayHeight = desc.runner.frame.displaySize.height,
                    .cpuTotalNs = apusr::profile::durationNs(cpuFrameBegin, cpuFrameEnd),
                    .cpuRecordNs = apusr::profile::durationNs(recordBegin, recordEnd),
                    .totalWorkgroups = totalWorkgroups,
                    .flags = profileDecision.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u,
                });
            }
            return Status::ok();
        }
#endif
        return fail("record_frame.command_list", Status::runtimeFailure(
            "DX12 FSR4 reference runtime requires a native command list to record dispatches"));
    }

private:
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    Status validateDeviceHealth(const char* stage) const
    {
        if (device_ == nullptr) {
            return Status::ok();
        }
        const HRESULT removedReason = device_->GetDeviceRemovedReason();
        if (SUCCEEDED(removedReason)) {
            return Status::ok();
        }
        std::ostringstream message;
        message << "DX12 FSR4 device became invalid during "
                << (stage != nullptr ? stage : "unknown stage")
                << "; GetDeviceRemovedReason=0x"
                << std::hex << static_cast<unsigned long>(removedReason)
                << "; device=" << static_cast<const void*>(device_.Get());
        return Status::runtimeFailure(message.str());
    }
#endif

    Status runtimePlanForFrame(
        const Dx12Fsr4ReferenceFrameDesc& desc,
        Dx12Fsr4ReferenceBackendPlan*& outPlan)
    {
        outPlan = nullptr;
        const Status inputStatus = validateDx12Fsr4ReferencePlanInputs(createDesc_, desc);
        if (!inputStatus.succeeded()) {
            return inputStatus;
        }
        // The graph, descriptor layout, shader selection, and dispatch records are
        // structural. Rebuilding those vectors every frame dominated the measured
        // DX12 upscaler CPU path, so keep them resident while only per-frame
        // constants and borrowed-resource realization change.
        const Dx12RuntimePlanStructureKey key = makeDx12RuntimePlanStructureKey(createDesc_, desc);
        if (!runtimePlanCacheValid_ || !(runtimePlanCacheKey_ == key)) {
            Dx12Fsr4ReferenceBackendPlan rebuiltPlan{};
            const Status planStatus = makeDx12Fsr4ReferenceBackendPlan(createDesc_, desc, rebuiltPlan);
            if (!planStatus.succeeded()) {
                return planStatus;
            }
            const Status reflectionStatus = validateReflectionManifestForPlan(rebuiltPlan);
            if (!reflectionStatus.succeeded()) {
                return reflectionStatus;
            }
            runtimePlanCache_ = std::move(rebuiltPlan);
            runtimePlanCacheKey_ = key;
            runtimePlanCacheValid_ = true;
            ++runtimePlanGeneration_;
        } else {
            refreshReferenceFramePlanPerFrameData(createDesc_.runner, desc.runner, runtimePlanCache_.framePlan);
            refreshResourceRealizationPerFrame(
                runtimePlanCache_.framePlan,
                runtimePlanCache_.resourceUsagePlan,
                desc,
                runtimePlanCache_.resourceRealizationPlan);
            refreshDescriptorRealizationPerFrame(
                runtimePlanCache_.resourceRealizationPlan,
                runtimePlanCache_.descriptorPatchPlan,
                runtimePlanCache_.descriptorRealizationPlan);
        }
        outPlan = &runtimePlanCache_;
        return Status::ok();
    }

    std::uint64_t profileConfigurationHash(
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc) const noexcept
    {
        std::uint64_t hash = 0x4150555352465352ull;
        hash = apusr::profile::hashCombine(hash, static_cast<std::uint64_t>(plan.framePlan.pipelineVariant));
        hash = apusr::profile::hashCombine(hash, static_cast<std::uint64_t>(plan.framePlan.preset));
        hash = apusr::profile::hashCombine(hash, static_cast<std::uint64_t>(plan.framePlan.frameCapacity));
        hash = apusr::profile::hashCombine(hash, plan.framePlan.permutationKey);
        hash = apusr::profile::hashCombine(hash,
            (static_cast<std::uint64_t>(frameDesc.runner.frame.renderSize.width) << 32u) | frameDesc.runner.frame.renderSize.height);
        hash = apusr::profile::hashCombine(hash,
            (static_cast<std::uint64_t>(frameDesc.runner.frame.displaySize.width) << 32u) | frameDesc.runner.frame.displaySize.height);
        hash = apusr::profile::hashCombine(hash, plan.framePlan.constants.rcasEnabled != 0u ? 1u : 0u);
        return hash;
    }

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    std::uint64_t profileResourceAllocationBytes(ID3D12Resource* resource) const noexcept
    {
        if (device_ == nullptr || resource == nullptr) {
            return 0u;
        }
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        return device_->GetResourceAllocationInfo(0u, 1u, &desc).SizeInBytes;
    }

    void recordOwnedResourceFreeEvents(const std::uint64_t frameId) noexcept
    {
        if (!profileStream_) {
            return;
        }
        for (const auto& [resourceId, resource] : ownedResources_) {
            ID3D12Resource* native = resource.Get();
            if (native == nullptr) {
                continue;
            }
            apusr::profile::recordResourceEvent({
                .stream = profileStream_,
                .frameId = frameId,
                .name = "upscaler.owned_resource",
                .detail = resourceIdName(static_cast<ResourceId>(resourceId)),
                .operation = apusr::profile::ResourceOperation::Free,
                .resourceId = resourceId,
                .nativeHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(native)),
                .bytes = profileResourceAllocationBytes(native),
            });
        }
    }

    void recordOwnedResourceAllocation(
        const std::uint64_t frameId,
        const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
        ID3D12Resource* native) noexcept
    {
        if (!profileStream_ || native == nullptr) {
            return;
        }
        const D3D12_RESOURCE_DESC desc = native->GetDesc();
        const D3D12_RESOURCE_ALLOCATION_INFO allocation = device_->GetResourceAllocationInfo(0u, 1u, &desc);
        apusr::profile::recordResourceEvent({
            .stream = profileStream_,
            .frameId = frameId,
            .name = "upscaler.owned_resource",
            .detail = resource.name != nullptr ? resource.name : resourceIdName(resource.id),
            .operation = apusr::profile::ResourceOperation::Allocate,
            .resourceId = static_cast<std::uint64_t>(resource.id),
            .nativeHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(native)),
            .bytes = allocation.SizeInBytes,
            .alignment = allocation.Alignment,
            .heapOrMemoryType = resource.ownership == Dx12Fsr4ReferenceResourceOwnership::UploadOwned
                ? static_cast<std::uint32_t>(D3D12_HEAP_TYPE_UPLOAD)
                : static_cast<std::uint32_t>(D3D12_HEAP_TYPE_DEFAULT),
            .flags = resource.temporal ? 1u : 0u,
        });
    }

    void resetFastResourceLookup() noexcept
    {
        realizedResourceById_.fill(nullptr);
        ownedNativeResourceById_.fill(nullptr);
        ownedResourceStateById_.fill(D3D12_RESOURCE_STATE_COMMON);
        ownedResourceStateValidById_.fill(false);
    }

    void rebuildFastResourceLookup(const Dx12Fsr4ReferenceResourceRealizationPlan& realization) noexcept
    {
        realizedResourceById_.fill(nullptr);
        for (const Dx12Fsr4ReferenceRealizedResourceDesc& resource : realization.resources) {
            realizedResourceById_[resourceLookupIndex(resource.id)] = &resource;
        }
        realizedResourceLookupGeneration_ = runtimePlanGeneration_;
    }

    const Dx12Fsr4ReferenceRealizedResourceDesc* realizedResourceForId(const ResourceId id) const noexcept
    {
        return realizedResourceById_[resourceLookupIndex(id)];
    }

    ID3D12Resource* ownedNativeResourceForId(const ResourceId id) const noexcept
    {
        return ownedNativeResourceById_[resourceLookupIndex(id)];
    }

    void unmapConstantUploadBuffer() noexcept
    {
        if (constantUploadBuffer_ != nullptr && constantUploadMappedPtr_ != nullptr) {
            constantUploadBuffer_->Unmap(0, nullptr);
        }
        constantUploadMappedPtr_ = nullptr;
        constantUploadPersistentlyMapped_ = false;
        currentConstantUploadGpuBase_ = 0;
    }


#if APUSR_ENABLE_GPU_DEBUG_MARKERS
    void nameBorrowedExternalResourcesForProfiling(
        const Dx12Fsr4ReferenceResourceRealizationPlan& realization) noexcept
    {
        if (!dx12GpuResourceNamesEnabled()) {
            return;
        }
        for (const Dx12Fsr4ReferenceRealizedResourceDesc& resource : realization.resources) {
            if (resource.ownership != Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed || !resource.validHandle) {
                continue;
            }
            auto* nativeResource = static_cast<ID3D12Resource*>(resource.nativeResource.value);
            if (nativeResource == nullptr) {
                continue;
            }
            const std::string profileName = dx12GpuProfileResourceName(resource);
            const auto inserted = profiledExternalResourceNames_.emplace(resource.nativeResource.value, profileName);
            if (inserted.second || inserted.first->second != profileName) {
                inserted.first->second = profileName;
                dx12SetGpuProfileObjectName(nativeResource, profileName);
            }
        }
    }
#endif

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
    void ensureGpuDebugAnnotationMode() noexcept
    {
        if (gpuDebugAnnotationModeAttempted_) {
            return;
        }
        gpuDebugAnnotationModeAttempted_ = true;
        if (!dx12GpuDebugMarkersEnabled()) {
            gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::NotRequested;
            return;
        }
#if APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API
        gpuDebugAnnotationFeatureCheckHr_ = S_OK;
        gpuDebugAnnotationQueryInterfaceHr_ = S_OK;
        if (device_ == nullptr) {
            gpuDebugAnnotationQueryInterfaceHr_ = E_POINTER;
            gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::QueryInterfaceFailed;
            return;
        }

        // Prefer the actual Tools2 interface as the authority. Some runtimes can expose
        // the interface even when CheckFeatureSupport does not understand the newer
        // feature enum cleanly. A failed feature probe is useful diagnostic data, but it
        // should not block the path if the driver-visible annotation interface exists.
        ComPtr<ID3D12DeviceTools2> deviceTools2;
        HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&deviceTools2));
        gpuDebugAnnotationQueryInterfaceHr_ = hr;
        if (SUCCEEDED(hr) && deviceTools2 != nullptr) {
            deviceTools2->SetUserDefinedAnnotationMode(D3D12_USER_DEFINED_ANNOTATION_MODE_DRIVER_RETAIL);
            gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::Configured;
            return;
        }

        D3D12_FEATURE_DATA_USER_DEFINED_ANNOTATION annotationSupport{};
        hr = device_->CheckFeatureSupport(
            D3D12_FEATURE_USER_DEFINED_ANNOTATION,
            &annotationSupport,
            sizeof(annotationSupport));
        gpuDebugAnnotationFeatureCheckHr_ = hr;
        if (FAILED(hr)) {
            gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::FeatureCheckFailed;
            return;
        }
        if (!annotationSupport.Supported) {
            gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::Unsupported;
            return;
        }
        gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::QueryInterfaceFailed;
#else
        gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::HeaderUnavailable;
#endif
    }

    std::string gpuProfileStatusNote(const char* prefix) const
    {
        std::ostringstream note;
        note << (prefix != nullptr ? prefix : "profile_status")
             << ";gpu_markers_compiled=1"
#if APUSR_ENABLE_WINPIX_EVENT_RUNTIME
             << ";winpix_compiled=1"
#else
             << ";winpix_compiled=0"
#endif
#if APUSR_WINPIX_EVENT_RUNTIME_DLL_AVAILABLE
             << ";winpix_dll_available=1"
#else
             << ";winpix_dll_available=0"
#endif
#if APUSR_D3D12_AGILITY_SDK_HEADERS
             << ";d3d12_agility_headers=1"
#else
             << ";d3d12_agility_headers=0"
#endif
#if APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API
             << ";d3d12_driver_retail_annotation_api=1"
#else
             << ";d3d12_driver_retail_annotation_api=0"
#endif
             << ";gpu_markers_env=" << (dx12GpuDebugMarkersEnabled() ? 1 : 0)
             << ";resource_names_env=" << (dx12GpuTimestampEnvTruth("APUSR_DX12_RESOURCE_NAMES") ? 1 : 0)
             << ";resource_names_enabled=" << (dx12GpuResourceNamesEnabled() ? 1 : 0)
             << ";annotation_mode=" << dx12GpuAnnotationModeStatusName(gpuDebugAnnotationModeStatus_);
#if APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API
        note << ";annotation_qi_hr=" << dx12HresultHex(gpuDebugAnnotationQueryInterfaceHr_)
             << ";annotation_feature_hr=" << dx12HresultHex(gpuDebugAnnotationFeatureCheckHr_);
#endif
        return note.str();
    }
#else
    std::string gpuProfileStatusNote(const char* prefix) const
    {
        std::ostringstream note;
        note << (prefix != nullptr ? prefix : "profile_status")
             << ";gpu_markers_compiled=0";
        return note.str();
    }
#endif

    void resetGpuTimestampProfiler() noexcept
    {
        gpuTimestampQueryHeap_.Reset();
        gpuTimestampReadback_.Reset();
        gpuTimestampFrames_.clear();
        gpuTimestampRingSlots_ = 0;
        gpuTimestampQueriesPerSlot_ = 0;
        gpuTimestampReadbackBytes_ = 0;
        gpuTimestampFrameSequence_ = 0;
        gpuTimestampCpuClockStarted_ = false;
        gpuTimestampCpuStartTime_ = std::chrono::steady_clock::time_point{};
// Do not reset GPU debug annotation status here. Timestamp query heaps can be
// recreated after ensureGpuDebugAnnotationMode() has already configured PIX/RGP
// user-marker annotation mode; clearing the cached status here makes the
// profiler reset CSV row report annotation_mode=not_requested even when marker
// configuration actually ran. The annotation state is tied to the D3D12 device,
// not to the timestamp-query heap lifecycle.
    }

    std::uint32_t gpuTimestampRingSlots() const noexcept
    {
        std::uint32_t slots = dx12GpuTimestampEnvU32("APUSR_DX12_GPU_TIMESTAMP_RING", 12u);
        if (slots < 4u) {
            slots = 4u;
        }
        if (slots > 64u) {
            slots = 64u;
        }
        return slots;
    }

    std::uint32_t gpuTimestampReadDelay() const noexcept
    {
        const std::uint32_t slots = gpuTimestampRingSlots_ != 0 ? gpuTimestampRingSlots_ : gpuTimestampRingSlots();
        std::uint32_t delay = dx12GpuTimestampEnvU32("APUSR_DX12_GPU_TIMESTAMP_READ_DELAY", 8u);
        if (delay >= slots) {
            delay = slots - 1u;
        }
        return delay;
    }

    std::string gpuTimestampTracePath() const
    {
        return dx12GpuTimestampDefaultTracePath();
    }

    void writeGpuTimestampCsvLine(const std::string& line) noexcept
    {
        if (!dx12LegacyGpuTimestampsEnabled()) {
            return;
        }
        try {
            const std::string path = gpuTimestampTracePath();
            const std::filesystem::path fsPath(path);
            if (fsPath.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(fsPath.parent_path(), ec);
            }
            const bool writeHeader = !gpuTimestampCsvHeaderWritten_;
            std::ofstream file(
                path,
                writeHeader ? (std::ios::out | std::ios::trunc) : (std::ios::out | std::ios::app));
            if (!file) {
                return;
            }
            if (writeHeader) {
                file << "cpu_seconds_since_start,cpu_frame_sequence,model_frame_index,slot,dispatch_index,dispatch_name,groups_x,groups_y,groups_z,ticks,microseconds,dispatch_ticks,dispatch_microseconds,setup_ticks,setup_microseconds,frame_percent,dispatch_frame_percent,timestamp_frequency_hz,query_count,srv_bindings,uav_bindings,srv_unique_bytes,uav_unique_bytes,srv_external_bytes,uav_external_bytes,srv_temporal_bytes,uav_temporal_bytes,scratch_srv_bytes,scratch_uav_bytes,notes\n";
                gpuTimestampCsvHeaderWritten_ = true;
            }
            file << line << '\n';
            file.flush();
        } catch (...) {
        }
    }

    Status ensureGpuTimestampProfilerCapacity(const Dx12Fsr4ReferenceBackendPlan& plan)
    {
        if (!dx12GpuTimestampsEnabled()) {
            return Status::ok();
        }
        if (device_ == nullptr) {
            return Status::invalidArgument("DX12 GPU timestamp profiling requires a native D3D12 device");
        }
#if APUSR_ENABLE_GPU_DEBUG_MARKERS
        ensureGpuDebugAnnotationMode();
#endif

        std::uint32_t enabledDispatches = 0;
        for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
            if (dispatch.enabled) {
                ++enabledDispatches;
            }
        }
        // Per slot: frame begin/end plus three timestamps per dispatch.
        // The dispatch begin timestamp is emitted after transitions/clears/root binding
        // and immediately before Dispatch(), allowing the CSV to separate pure shader
        // execution from resource-state/clear/setup GPU work.
        const std::uint32_t requiredQueriesPerSlot = 2u + enabledDispatches * 3u;
        const std::uint32_t requiredRingSlots = gpuTimestampRingSlots();
        if (requiredQueriesPerSlot == 0) {
            return Status::ok();
        }
        if (gpuTimestampQueryHeap_ != nullptr && gpuTimestampReadback_ != nullptr &&
            gpuTimestampRingSlots_ == requiredRingSlots && gpuTimestampQueriesPerSlot_ >= requiredQueriesPerSlot) {
            return Status::ok();
        }

        resetGpuTimestampProfiler();
        gpuTimestampRingSlots_ = requiredRingSlots;
        gpuTimestampQueriesPerSlot_ = requiredQueriesPerSlot;
        const std::uint32_t queryCount = gpuTimestampRingSlots_ * gpuTimestampQueriesPerSlot_;
        gpuTimestampReadbackBytes_ = static_cast<std::uint64_t>(queryCount) * sizeof(std::uint64_t);

        D3D12_QUERY_HEAP_DESC queryHeapDesc{};
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count = queryCount;
        queryHeapDesc.NodeMask = 0;
        HRESULT hr = device_->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&gpuTimestampQueryHeap_));
        if (FAILED(hr)) {
            const HRESULT removedReason = device_->GetDeviceRemovedReason();
            resetGpuTimestampProfiler();
            std::ostringstream message;
            message << "ID3D12Device::CreateQueryHeap(DX12 FSR4 timestamps) failed with HRESULT 0x"
                    << std::hex << static_cast<unsigned long>(hr)
                    << "; GetDeviceRemovedReason=0x"
                    << std::hex << static_cast<unsigned long>(removedReason)
                    << "; device=" << static_cast<const void*>(device_.Get());
            return Status::runtimeFailure(message.str());
        }

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Alignment = 0;
        bufferDesc.Width = gpuTimestampReadbackBytes_;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        hr = device_->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&gpuTimestampReadback_));
        if (FAILED(hr)) {
            resetGpuTimestampProfiler();
            return hresultStatus("ID3D12Device::CreateCommittedResource(DX12 FSR4 timestamp readback)", hr);
        }

        gpuTimestampFrames_.assign(gpuTimestampRingSlots_, Dx12GpuTimestampFrameMetadata{});
        writeGpuTimestampCsvLine(std::string("0,0,0,0,0,__profiler_reset__,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,") + gpuProfileStatusNote("query_heap_recreated"));
        return Status::ok();
    }

    void logResolvedGpuTimestampSlot(const std::uint32_t slot) noexcept
    {
        if (!dx12GpuTimestampsEnabled() || gpuTimestampReadback_ == nullptr || slot >= gpuTimestampFrames_.size()) {
            return;
        }
        Dx12GpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        if (!metadata.valid || metadata.logged || metadata.resolvedQueryCount == 0 || metadata.resolvedQueryCount > gpuTimestampQueriesPerSlot_) {
            return;
        }

        const std::uint64_t byteOffset = static_cast<std::uint64_t>(slot) * gpuTimestampQueriesPerSlot_ * sizeof(std::uint64_t);
        const std::uint64_t byteCount = static_cast<std::uint64_t>(metadata.resolvedQueryCount) * sizeof(std::uint64_t);
        void* mapped = nullptr;
        D3D12_RANGE readRange{};
        readRange.Begin = static_cast<SIZE_T>(byteOffset);
        readRange.End = static_cast<SIZE_T>(byteOffset + byteCount);
        const HRESULT hr = gpuTimestampReadback_->Map(0, &readRange, &mapped);
        if (FAILED(hr) || mapped == nullptr) {
            return;
        }

        const auto* timestamps = reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(mapped) + byteOffset);
        const std::uint64_t frameBegin = timestamps[metadata.frameBeginQueryOffset];
        const std::uint64_t frameEnd = timestamps[metadata.frameEndQueryOffset];
        const std::uint64_t frameTicks = frameEnd >= frameBegin ? frameEnd - frameBegin : 0ull;
        const double frequency = static_cast<double>(metadata.timestampFrequency);
        const auto usFromTicks = [frequency](const std::uint64_t ticks) -> double {
            return frequency > 0.0 ? (static_cast<double>(ticks) * 1000000.0) / frequency : 0.0;
        };
        const auto nsFromTicks = [frequency](const std::uint64_t ticks) -> std::uint64_t {
            if (frequency <= 0.0) {
                return 0u;
            }
            const long double value = (static_cast<long double>(ticks) * 1000000000.0L) / static_cast<long double>(frequency);
            return static_cast<std::uint64_t>(value);
        };
        const bool legacyProfile = dx12LegacyGpuTimestampsEnabled();
        const auto appendLine = [&](const std::uint32_t dispatchIndex,
                                    const std::string& dispatchName,
                                    const std::uint32_t groupsX,
                                    const std::uint32_t groupsY,
                                    const std::uint32_t groupsZ,
                                    const std::uint64_t ticks,
                                    const std::uint64_t dispatchTicks,
                                    const std::uint64_t setupTicks,
                                    const double percent,
                                    const double dispatchPercent,
                                    const std::uint32_t srvBindingCount,
                                    const std::uint32_t uavBindingCount,
                                    const std::uint64_t srvUniqueBytes,
                                    const std::uint64_t uavUniqueBytes,
                                    const std::uint64_t srvExternalBytes,
                                    const std::uint64_t uavExternalBytes,
                                    const std::uint64_t srvTemporalBytes,
                                    const std::uint64_t uavTemporalBytes,
                                    const std::uint64_t scratchSrvBytes,
                                    const std::uint64_t scratchUavBytes,
                                    const char* notes) {
            std::ostringstream line;
            line << metadata.cpuSecondsSinceStart << ','
                 << metadata.cpuFrameSequence << ','
                 << metadata.modelFrameIndex << ','
                 << metadata.slot << ','
                 << dispatchIndex << ','
                 << dispatchName << ','
                 << groupsX << ','
                 << groupsY << ','
                 << groupsZ << ','
                 << ticks << ','
                 << usFromTicks(ticks) << ','
                 << dispatchTicks << ','
                 << usFromTicks(dispatchTicks) << ','
                 << setupTicks << ','
                 << usFromTicks(setupTicks) << ','
                 << percent << ','
                 << dispatchPercent << ','
                 << metadata.timestampFrequency << ','
                 << metadata.resolvedQueryCount << ','
                 << srvBindingCount << ','
                 << uavBindingCount << ','
                 << srvUniqueBytes << ','
                 << uavUniqueBytes << ','
                 << srvExternalBytes << ','
                 << uavExternalBytes << ','
                 << srvTemporalBytes << ','
                 << uavTemporalBytes << ','
                 << scratchSrvBytes << ','
                 << scratchUavBytes << ','
                 << notes;
            writeGpuTimestampCsvLine(line.str());
        };

        if (legacyProfile) {
            appendLine(0u, "__frame_total__", 0u, 0u, 0u, frameTicks, 0ull, 0ull, frameTicks != 0 ? 100.0 : 0.0, 0.0, 0u, 0u, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, "frame_begin_to_frame_end");
        }
        if (metadata.unifiedProfile) {
            std::uint64_t totalWorkgroups = 0u;
            for (const Dx12GpuTimestampDispatchMetadata& dispatch : metadata.dispatches) {
                totalWorkgroups += static_cast<std::uint64_t>(dispatch.groupsX) * dispatch.groupsY * dispatch.groupsZ;
            }
            apusr::profile::recordFrameSample(apusr::profile::FrameSample{
                .stream = profileStream_,
                .frameId = metadata.modelFrameIndex,
                .sampleSequence = metadata.sampleSequence,
                .burstSequence = metadata.burstSequence,
                .configurationHash = metadata.configurationHash,
                .burstFrame = metadata.burstFrame,
                .renderWidth = metadata.renderWidth,
                .renderHeight = metadata.renderHeight,
                .displayWidth = metadata.displayWidth,
                .displayHeight = metadata.displayHeight,
                .gpuDurationNs = nsFromTicks(frameTicks),
                .cpuTotalNs = metadata.cpuTotalNs,
                .cpuRecordNs = metadata.cpuRecordNs,
                .totalWorkgroups = totalWorkgroups,
                .gpuBeginTick = frameBegin,
                .gpuEndTick = frameEnd,
                .gpuFrequency = metadata.timestampFrequency,
                .nativeQueue = metadata.nativeQueue,
                .flags = metadata.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u,
            });
        }
        for (std::uint32_t i = 0; i < metadata.dispatches.size(); ++i) {
            const Dx12GpuTimestampDispatchMetadata& dispatch = metadata.dispatches[i];
            if (dispatch.queryBeginOffset >= metadata.resolvedQueryCount || dispatch.queryDispatchBeginOffset >= metadata.resolvedQueryCount || dispatch.queryEndOffset >= metadata.resolvedQueryCount) {
                continue;
            }
            const std::uint64_t begin = timestamps[dispatch.queryBeginOffset];
            const std::uint64_t dispatchBegin = timestamps[dispatch.queryDispatchBeginOffset];
            const std::uint64_t end = timestamps[dispatch.queryEndOffset];
            const std::uint64_t ticks = end >= begin ? end - begin : 0ull;
            const std::uint64_t dispatchTicks = end >= dispatchBegin ? end - dispatchBegin : 0ull;
            const std::uint64_t setupTicks = dispatchBegin >= begin ? dispatchBegin - begin : 0ull;
            if (legacyProfile) {
                const double percent = frameTicks != 0 ? (static_cast<double>(ticks) * 100.0) / static_cast<double>(frameTicks) : 0.0;
                const double dispatchPercent = frameTicks != 0 ? (static_cast<double>(dispatchTicks) * 100.0) / static_cast<double>(frameTicks) : 0.0;
                appendLine(
                    i + 1u,
                    dispatch.name,
                    dispatch.groupsX,
                    dispatch.groupsY,
                    dispatch.groupsZ,
                    ticks,
                    dispatchTicks,
                    setupTicks,
                    percent,
                    dispatchPercent,
                    dispatch.srvBindingCount,
                    dispatch.uavBindingCount,
                    dispatch.srvUniqueBytes,
                    dispatch.uavUniqueBytes,
                    dispatch.srvExternalBytes,
                    dispatch.uavExternalBytes,
                    dispatch.srvTemporalBytes,
                    dispatch.uavTemporalBytes,
                    dispatch.scratchSrvBytes,
                    dispatch.scratchUavBytes,
                    "transition_clear_bind_dispatch_block");
            }
            if (metadata.unifiedProfile) {
                apusr::profile::recordGpuInterval(apusr::profile::GpuInterval{
                    .stream = profileStream_,
                    .frameId = metadata.modelFrameIndex,
                    .sampleSequence = metadata.sampleSequence,
                    .name = dispatch.name.c_str(),
                    .durationNs = nsFromTicks(ticks),
                    .setupNs = nsFromTicks(setupTicks),
                    .executionNs = nsFromTicks(dispatchTicks),
                    .readBytes = dispatch.srvUniqueBytes,
                    .writeBytes = dispatch.uavUniqueBytes,
                    .groupsX = dispatch.groupsX,
                    .groupsY = dispatch.groupsY,
                    .groupsZ = dispatch.groupsZ,
                    .profileLevel = dispatch.profileLevel,
                    .descriptorCount = dispatch.srvBindingCount + dispatch.uavBindingCount,
                    .gpuBeginTick = begin,
                    .gpuExecutionBeginTick = dispatchBegin,
                    .gpuEndTick = end,
                    .gpuFrequency = metadata.timestampFrequency,
                    .nativeQueue = metadata.nativeQueue,
                });
            }
        }

        D3D12_RANGE writtenRange{};
        gpuTimestampReadback_->Unmap(0, &writtenRange);
        metadata.logged = true;
    }

    double gpuTimestampCpuSecondsSinceStart() noexcept
    {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point now = Clock::now();
        if (!gpuTimestampCpuClockStarted_) {
            gpuTimestampCpuClockStarted_ = true;
            gpuTimestampCpuStartTime_ = now;
            return 0.0;
        }
        return std::chrono::duration<double>(now - gpuTimestampCpuStartTime_).count();
    }

    void fillGpuTimestampDispatchDataflow(
        Dx12GpuTimestampDispatchMetadata& metadata,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const ReferenceDispatchDesc& dispatch) const noexcept
    {
        std::array<ResourceId, 32> srvResources{};
        std::array<ResourceId, 32> uavResources{};
        std::uint32_t srvResourceCount = 0;
        std::uint32_t uavResourceCount = 0;

        const auto addUniqueResource = [](std::array<ResourceId, 32>& resources,
                                          std::uint32_t& resourceCount,
                                          const ResourceId resource) noexcept -> bool {
            if (resource == ResourceId::Null) {
                return false;
            }
            for (std::uint32_t i = 0; i < resourceCount; ++i) {
                if (resources[i] == resource) {
                    return false;
                }
            }
            if (resourceCount >= resources.size()) {
                return false;
            }
            resources[resourceCount++] = resource;
            return true;
        };

        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (!patchMatchesDispatch(patch, dispatch)) {
                continue;
            }
            if (patch.space != Dx12Fsr4ReferenceDescriptorSpace::Srv && patch.space != Dx12Fsr4ReferenceDescriptorSpace::Uav) {
                continue;
            }
            const ResourceId resourceId = patch.resolvedResource != ResourceId::Null ? patch.resolvedResource : patch.resource;
            const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(resourceId);
            if (resource == nullptr) {
                continue;
            }
            const std::uint64_t bytes = resourceCommittedByteEstimate(*resource);
            const bool external = resource->ownership == Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed;
            const bool temporal = resource->temporal;
            const bool scratch = resourceId == ResourceId::ScratchBuffer || resourceId == ResourceId::BufferNhwcInputs || resourceId == ResourceId::BufferFusedQuantizedNhwcOutput;
            if (patch.space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
                ++metadata.srvBindingCount;
                if (!addUniqueResource(srvResources, srvResourceCount, resourceId)) {
                    continue;
                }
                metadata.srvUniqueBytes += bytes;
                if (external) {
                    metadata.srvExternalBytes += bytes;
                }
                if (temporal) {
                    metadata.srvTemporalBytes += bytes;
                }
                if (scratch) {
                    metadata.scratchSrvBytes += bytes;
                }
            } else {
                ++metadata.uavBindingCount;
                if (!addUniqueResource(uavResources, uavResourceCount, resourceId)) {
                    continue;
                }
                metadata.uavUniqueBytes += bytes;
                if (external) {
                    metadata.uavExternalBytes += bytes;
                }
                if (temporal) {
                    metadata.uavTemporalBytes += bytes;
                }
                if (scratch) {
                    metadata.scratchUavBytes += bytes;
                }
            }
        }
    }

    bool ensureProfileAdapter() noexcept
    {
        if (profileAdapter_)
            return true;
        if (!device_)
            return false;

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        const LUID adapterLuid = device_->GetAdapterLuid();
        return SUCCEEDED(factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&profileAdapter_)));
    }

    void recordVideoMemoryBudget(
        const std::uint64_t frameId,
        const std::uint32_t burstFrame) noexcept
    {
        if (burstFrame != 0u || !profileStream_ || !ensureProfileAdapter()) {
            if (burstFrame == 0u && profileStream_ && !profileAdapter_ && !profileMemoryBudgetUnavailableLogged_) {
                profileMemoryBudgetUnavailableLogged_ = true;
                apusr::profile::recordInstant(profileStream_, frameId, "memory.budget_unavailable");
            }
            return;
        }

        struct SegmentTotals {
            std::uint64_t usage = 0u;
            std::uint64_t budget = 0u;
            std::uint64_t reservation = 0u;
            std::uint64_t availableForReservation = 0u;
            std::uint32_t validNodes = 0u;
        };
        const auto querySegment = [&](const DXGI_MEMORY_SEGMENT_GROUP group) noexcept {
            SegmentTotals totals{};
            const UINT nodeCount = device_->GetNodeCount();
            for (UINT node = 0u; node < nodeCount; ++node) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (FAILED(profileAdapter_->QueryVideoMemoryInfo(node, group, &info)))
                    continue;
                totals.usage += info.CurrentUsage;
                totals.budget += info.Budget;
                totals.reservation += info.CurrentReservation;
                totals.availableForReservation += info.AvailableForReservation;
                ++totals.validNodes;
            }
            return totals;
        };

        const SegmentTotals local = querySegment(DXGI_MEMORY_SEGMENT_GROUP_LOCAL);
        const SegmentTotals nonlocal = querySegment(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL);
        const auto emit = [&](const char* budgetName, const char* reservationName, const SegmentTotals& totals) noexcept {
            if (totals.validNodes == 0u)
                return;
            apusr::profile::recordCounter(apusr::profile::Counter{
                .stream = profileStream_,
                .frameId = frameId,
                .name = budgetName,
                .value = totals.usage,
                .value2 = totals.budget,
                .flags = totals.validNodes,
            });
            apusr::profile::recordCounter(apusr::profile::Counter{
                .stream = profileStream_,
                .frameId = frameId,
                .name = reservationName,
                .value = totals.reservation,
                .value2 = totals.availableForReservation,
                .flags = totals.validNodes,
            });
        };
        emit("memory.budget.local", "memory.reservation.local", local);
        emit("memory.budget.nonlocal", "memory.reservation.nonlocal", nonlocal);
    }

    std::uint64_t timestampFrequencyForFrame(const Dx12Fsr4ReferenceFrameDesc& frameDesc) const noexcept
    {
        auto* queue = static_cast<ID3D12CommandQueue*>(frameDesc.commandQueue.value);
        if (queue == nullptr) {
            return 0ull;
        }
        UINT64 frequency = 0;
        const HRESULT hr = queue->GetTimestampFrequency(&frequency);
        return SUCCEEDED(hr) ? static_cast<std::uint64_t>(frequency) : 0ull;
    }

    void beginGpuTimestampFrame(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        const apusr::profile::SampleDecision& profileDecision,
        const std::uint64_t configurationHash,
        std::uint32_t& outSlot,
        std::uint32_t& outQueryCursor) noexcept
    {
        outSlot = std::numeric_limits<std::uint32_t>::max();
        outQueryCursor = 0;
        if (!dx12GpuTimestampsEnabled() || commandList == nullptr || gpuTimestampQueryHeap_ == nullptr || gpuTimestampQueriesPerSlot_ == 0 || gpuTimestampRingSlots_ == 0) {
            return;
        }

        const bool legacyCapture = dx12LegacyGpuTimestampsEnabled();
        const bool unifiedGpuCapture = profileDecision.capture && apusr::profile::gpuEnabled();
        if (!legacyCapture && !unifiedGpuCapture) {
            return;
        }

        const std::uint32_t readDelay = gpuTimestampReadDelay();
        if (gpuTimestampFrameSequence_ >= readDelay) {
            const std::uint32_t readSlot = static_cast<std::uint32_t>((gpuTimestampFrameSequence_ - readDelay) % gpuTimestampRingSlots_);
            logResolvedGpuTimestampSlot(readSlot);
        }

        outSlot = static_cast<std::uint32_t>(gpuTimestampFrameSequence_ % gpuTimestampRingSlots_);
        ++gpuTimestampFrameSequence_;
        const std::uint32_t queryBase = outSlot * gpuTimestampQueriesPerSlot_;
        Dx12GpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[outSlot];
        metadata = Dx12GpuTimestampFrameMetadata{};
        metadata.valid = true;
        metadata.unifiedProfile = unifiedGpuCapture;
        metadata.configurationChanged = profileDecision.configurationChanged;
        metadata.cpuFrameSequence = gpuTimestampFrameSequence_;
        metadata.modelFrameIndex = frameDesc.runner.frame.frameIndex;
        metadata.sampleSequence = profileDecision.sampleSequence;
        metadata.burstSequence = profileDecision.burstSequence;
        metadata.burstFrame = profileDecision.burstFrame;
        metadata.configurationHash = configurationHash;
        metadata.renderWidth = frameDesc.runner.frame.renderSize.width;
        metadata.renderHeight = frameDesc.runner.frame.renderSize.height;
        metadata.displayWidth = frameDesc.runner.frame.displaySize.width;
        metadata.displayHeight = frameDesc.runner.frame.displaySize.height;
        metadata.slot = outSlot;
        metadata.cpuSecondsSinceStart = gpuTimestampCpuSecondsSinceStart();
        metadata.timestampFrequency = timestampFrequencyForFrame(frameDesc);
        metadata.nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(frameDesc.commandQueue.value));
        metadata.dispatches.reserve(plan.framePlan.dispatches.size());
        if (metadata.unifiedProfile && metadata.timestampFrequency != 0u) {
            auto* queue = static_cast<ID3D12CommandQueue*>(frameDesc.commandQueue.value);
            UINT64 gpuCalibration = 0u;
            UINT64 cpuCalibration = 0u;
            if (queue != nullptr && SUCCEEDED(queue->GetClockCalibration(&gpuCalibration, &cpuCalibration))) {
                apusr::profile::recordCalibration(apusr::profile::ClockCalibration{
                    .stream = profileStream_,
                    .frameId = metadata.modelFrameIndex,
                    .sampleSequence = metadata.sampleSequence,
                    .cpuTick = static_cast<std::uint64_t>(cpuCalibration),
                    .gpuTick = static_cast<std::uint64_t>(gpuCalibration),
                    .gpuFrequency = metadata.timestampFrequency,
                    .nativeQueue = metadata.nativeQueue,
                });
            }
        }
        metadata.frameBeginQueryOffset = outQueryCursor++;
        commandList->EndQuery(gpuTimestampQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + metadata.frameBeginQueryOffset);
    }

    void markGpuTimestampDispatchBegin(
        ID3D12GraphicsCommandList* commandList,
        const std::uint32_t slot,
        std::uint32_t& queryCursor,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const ReferenceDispatchDesc& dispatch) noexcept
    {
        if (!dx12GpuTimestampsEnabled() || commandList == nullptr || slot == std::numeric_limits<std::uint32_t>::max() || slot >= gpuTimestampFrames_.size()) {
            return;
        }
        if (queryCursor + 3u > gpuTimestampQueriesPerSlot_) {
            return;
        }
        Dx12GpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        Dx12GpuTimestampDispatchMetadata dispatchMetadata{};
        dispatchMetadata.name = dispatch.name != nullptr ? dispatch.name : "<unnamed>";
        dispatchMetadata.groupsX = dispatch.grid.groupsX;
        dispatchMetadata.groupsY = dispatch.grid.groupsY;
        dispatchMetadata.groupsZ = dispatch.grid.groupsZ;
        dispatchMetadata.profileLevel = dispatch.logicalStage;
        fillGpuTimestampDispatchDataflow(dispatchMetadata, plan, dispatch);
        dispatchMetadata.queryBeginOffset = queryCursor++;
        dispatchMetadata.queryDispatchBeginOffset = queryCursor++;
        dispatchMetadata.queryEndOffset = queryCursor++;
        const std::uint32_t queryBase = slot * gpuTimestampQueriesPerSlot_;
        commandList->EndQuery(gpuTimestampQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + dispatchMetadata.queryBeginOffset);
        metadata.dispatches.push_back(std::move(dispatchMetadata));
    }


    void markGpuTimestampDispatchBodyBegin(ID3D12GraphicsCommandList* commandList, const std::uint32_t slot) noexcept
    {
        if (!dx12GpuTimestampsEnabled() || commandList == nullptr || slot == std::numeric_limits<std::uint32_t>::max() || slot >= gpuTimestampFrames_.size()) {
            return;
        }
        Dx12GpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        if (metadata.dispatches.empty()) {
            return;
        }
        const Dx12GpuTimestampDispatchMetadata& dispatch = metadata.dispatches.back();
        const std::uint32_t queryBase = slot * gpuTimestampQueriesPerSlot_;
        commandList->EndQuery(gpuTimestampQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + dispatch.queryDispatchBeginOffset);
    }

    void markGpuTimestampDispatchEnd(ID3D12GraphicsCommandList* commandList, const std::uint32_t slot) noexcept
    {
        if (!dx12GpuTimestampsEnabled() || commandList == nullptr || slot == std::numeric_limits<std::uint32_t>::max() || slot >= gpuTimestampFrames_.size()) {
            return;
        }
        Dx12GpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        if (metadata.dispatches.empty()) {
            return;
        }
        const Dx12GpuTimestampDispatchMetadata& dispatch = metadata.dispatches.back();
        const std::uint32_t queryBase = slot * gpuTimestampQueriesPerSlot_;
        commandList->EndQuery(gpuTimestampQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + dispatch.queryEndOffset);
    }

    void endGpuTimestampFrame(ID3D12GraphicsCommandList* commandList, const std::uint32_t slot, std::uint32_t& queryCursor) noexcept
    {
        if (!dx12GpuTimestampsEnabled() || commandList == nullptr || slot == std::numeric_limits<std::uint32_t>::max() || slot >= gpuTimestampFrames_.size()) {
            return;
        }
        if (queryCursor >= gpuTimestampQueriesPerSlot_) {
            return;
        }
        Dx12GpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        metadata.frameEndQueryOffset = queryCursor++;
        metadata.resolvedQueryCount = queryCursor;
        metadata.dispatchCount = static_cast<std::uint32_t>(metadata.dispatches.size());
        const std::uint32_t queryBase = slot * gpuTimestampQueriesPerSlot_;
        const std::uint64_t byteOffset = static_cast<std::uint64_t>(queryBase) * sizeof(std::uint64_t);
        commandList->EndQuery(gpuTimestampQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + metadata.frameEndQueryOffset);
        commandList->ResolveQueryData(
            gpuTimestampQueryHeap_.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            queryBase,
            metadata.resolvedQueryCount,
            gpuTimestampReadback_.Get(),
            byteOffset);
    }
#endif

    Status validateReflectionManifestForPlan(const Dx12Fsr4ReferenceBackendPlan& plan)
    {
        if (!createDesc_.enableReflectionManifestValidation || !reflectionManifestLoaded_) {
            return Status::ok();
        }
        const std::uint64_t key = reflectionValidationCacheKeyForPlan(plan.framePlan);
        if (key == reflectionValidationCacheKey_) {
            return Status::ok();
        }
        const Status status = validateDx12Fsr4ReferenceShaderReflectionManifestCoverage(
            plan,
            reflectionManifest_,
            createDesc_.requireCompleteReflectionCoverage,
            nullptr);
        if (status.succeeded()) {
            reflectionValidationCacheKey_ = key;
        }
        return status;
    }

    Status prepareInitializerUploadForPreset(const Preset preset, const bool forceUpload)
    {
        const bool needBytes = forceUpload || !initializerPresetValid_ || initializerPreset_ != preset || initializerBytes_.empty();
        if (needBytes) {
            std::vector<std::uint8_t> initializerBytes;
            const Status initializerStatus = loadReferenceInitializerBytes(createDesc_.repoRoot, preset, initializerBytes);
            if (!initializerStatus.succeeded()) {
                if (createDesc_.requireInitializerBytes) {
                    return initializerStatus;
                }
                initializerBytes.clear();
            }
            initializerBytes_ = std::move(initializerBytes);
            initializerPreset_ = preset;
            initializerPresetValid_ = true;
        }

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        if (device_ != nullptr) {
            const auto initializerUpload = ownedResources_.find(static_cast<std::uint32_t>(ResourceId::InitializerUploadBuffer));
            if (initializerUpload != ownedResources_.end() && !initializerBytes_.empty() && (needBytes || forceUpload)) {
                const Status writeStatus = writeUploadBufferBytes(
                    initializerUpload->second.Get(),
                    0,
                    initializerBytes_.data(),
                    static_cast<std::uint64_t>(initializerBytes_.size()));
                if (!writeStatus.succeeded()) {
                    return writeStatus;
                }
                pendingResourceInitialization_ = true;
            }
        }
#else
        (void)forceUpload;
#endif
        return Status::ok();
    }

    Status prepareResources(const Dx12Fsr4ReferenceBackendPlan& plan, const Dx12Fsr4ReferenceFrameDesc& frameDesc)
    {
        const Dx12Fsr4ReferenceResourceRealizationPlan& realization = plan.resourceRealizationPlan;
        const std::uint64_t resourceCacheKey = resourceCacheKeyForPlan(plan.resourcePlan);
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        if (realizedResourceLookupGeneration_ != runtimePlanGeneration_) {
            rebuildFastResourceLookup(realization);
        }
#endif

        // Planning and tests can run without native resource handles.  Once a
        // real command list is supplied, required borrowed inputs/outputs must
        // be present before we touch descriptors or barriers.
        if (frameDesc.commandList.value != nullptr && realization.missingRequiredExternalCount != 0) {
            std::ostringstream message;
            message << "DX12 FSR4 reference frame is missing "
                    << realization.missingRequiredExternalCount
                    << " required external resource handle(s)";
            return Status::invalidArgument(message.str());
        }

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
        nameBorrowedExternalResourcesForProfiling(realization);
#endif

        if (preparedResourceCacheKey_ == resourceCacheKey) {
            return prepareInitializerUploadForPreset(plan.framePlan.preset, false);
        }

        defaultExposureBytes_ = std::array<float, 2>{1.0f, 1.0f};

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        recordOwnedResourceFreeEvents(frameDesc.runner.frame.frameIndex);
        ownedResources_.clear();
        ownedNativeResourceById_.fill(nullptr);
        ownedResourceStateById_.fill(D3D12_RESOURCE_STATE_COMMON);
        ownedResourceStateValidById_.fill(false);
        descriptorTemplateHeap_.Reset();
        descriptorTemplateDescriptorsPerFrame_ = 0;
        descriptorTemplateResourceGeneration_ = std::numeric_limits<std::uint64_t>::max();
        descriptorTemplatePlanKey_ = std::numeric_limits<std::uint64_t>::max();
        defaultExposureUploadBuffer_.Reset();
        defaultExposureUploadFootprint_ = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
        pendingResourceInitialization_ = true;
        if (device_ != nullptr) {
            for (const Dx12Fsr4ReferenceRealizedResourceDesc& resource : realization.resources) {
                if (resource.ownership != Dx12Fsr4ReferenceResourceOwnership::InternalOwned &&
                    resource.ownership != Dx12Fsr4ReferenceResourceOwnership::UploadOwned) {
                    continue;
                }
                ComPtr<ID3D12Resource> nativeResource;
                const Status createStatus = createOwnedDx12Resource(device_.Get(), resource, nativeResource);
                if (!createStatus.succeeded()) {
                    return createStatus;
                }
                const std::size_t resourceSlot = resourceLookupIndex(resource.id);
                ID3D12Resource* rawResource = nativeResource.Get();
                const D3D12_RESOURCE_STATES initialState = initialDx12StateForOwnedResource(resource);
                ownedResourceStateById_[resourceSlot] = initialState;
                ownedResourceStateValidById_[resourceSlot] = true;
                recordOwnedResourceAllocation(frameDesc.runner.frame.frameIndex, resource, rawResource);
                ownedResources_.emplace(static_cast<std::uint32_t>(resource.id), std::move(nativeResource));
                ownedNativeResourceById_[resourceSlot] = rawResource;
            }

            const auto* defaultExposure = realizedResourceForId(ResourceId::InternalDefaultExposure);
            if (defaultExposure != nullptr && defaultExposure->initialContents == ResourceInitialContents::DefaultExposureOneOne) {
                const D3D12_RESOURCE_DESC desc = dx12ResourceDesc(*defaultExposure);
                UINT64 uploadBytes = 0;
                UINT rowCount = 0;
                UINT64 rowBytes = 0;
                device_->GetCopyableFootprints(&desc, 0, 1, 0, &defaultExposureUploadFootprint_, &rowCount, &rowBytes, &uploadBytes);
                const Status uploadStatus = createUploadBufferResource(
                    device_.Get(),
                    uploadBytes,
                    "APUSR FSR4 reference default exposure upload",
                    defaultExposureUploadBuffer_);
                if (!uploadStatus.succeeded()) {
                    return uploadStatus;
                }
                if (defaultExposureUploadBuffer_ != nullptr) {
                    void* mapped = nullptr;
                    D3D12_RANGE readRange{};
                    const HRESULT mapHr = defaultExposureUploadBuffer_->Map(0, &readRange, &mapped);
                    if (FAILED(mapHr)) {
                        return hresultStatus("ID3D12Resource::Map(FSR4 default exposure upload)", mapHr);
                    }
                    std::memset(mapped, 0, static_cast<std::size_t>(uploadBytes));
                    std::memcpy(static_cast<std::uint8_t*>(mapped) + defaultExposureUploadFootprint_.Offset, defaultExposureBytes_.data(), sizeof(float) * 2u);
                    D3D12_RANGE writtenRange{0, static_cast<SIZE_T>(uploadBytes)};
                    defaultExposureUploadBuffer_->Unmap(0, &writtenRange);
                }
            }
        }
#endif
        preparedResourceCacheKey_ = resourceCacheKey;
        ++resourceGeneration_;
        return prepareInitializerUploadForPreset(plan.framePlan.preset, true);
    }

    Status preparePipelines(const Dx12Fsr4ReferenceBackendPlan& plan)
    {
        const std::uint64_t pipelinePlanKey = reflectionValidationCacheKeyForPlan(plan.framePlan);
        if (preparedPipelinePlanKey_ == pipelinePlanKey) {
            return Status::ok();
        }
        const Status providerStatus = validateShaderProviderPlan(plan);
        if (!providerStatus.succeeded()) {
            return providerStatus;
        }
        preparedShaderBlobCount_ = plan.shaderProvider.totalResolvedShaderBlobCount;

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        if (device_ == nullptr) {
            preparedPipelinePlanKey_ = pipelinePlanKey;
            return Status::ok();
        }
        if (rootSignature_ == nullptr) {
            return Status::invalidArgument("DX12 FSR4 reference runtime has a device but no root signature for PSO creation");
        }

        for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
            if (!dispatch.enabled) {
                continue;
            }
            const std::string name = pipelineCacheKeyForDispatch(plan.framePlan, dispatch);
            if (pipelineStates_.find(name) != pipelineStates_.end()) {
                continue;
            }
            ResolvedShaderBytecode bytecode{};
            const Status bytecodeStatus = resolveShaderBytecode(
                plan.shaderProvider.config,
                plan.framePlan,
                dispatch,
                bytecode);
            if (!bytecodeStatus.succeeded()) {
                return bytecodeStatus;
            }
            ComPtr<ID3D12PipelineState> pso;
            const fsr4_reference_dx12_shader_blobs::ShaderBlob blobView{
                bytecode.dispatchName,
                plan.framePlan.preset,
                plan.framePlan.frameCapacity,
                plan.framePlan.permutationKey,
                dispatch.kind,
                dispatch.logicalStage,
                bytecode.data,
                bytecode.size,
            };
            const Status psoStatus = createComputePipelineObject(device_.Get(), rootSignature_.Get(), blobView, pso);
            if (!psoStatus.succeeded()) {
                return psoStatus;
            }
            if (profileStream_ && apusr::profile::gpuEnabled()) {
                std::uint64_t workloadId = apusr::profile::hashString(name);
                workloadId = apusr::profile::hashCombine(workloadId, plan.framePlan.permutationKey);
                apusr::profile::recordGpuWorkloadInfo(apusr::profile::GpuWorkloadInfo{
                    .stream = profileStream_,
                    .name = bytecode.dispatchName != nullptr ? bytecode.dispatchName : "fsr4.dispatch",
                    .detail = "dxil.compute",
                    .workloadId = workloadId,
                    .nativePipeline = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pso.Get())),
                    .codeHash = apusr::profile::hashString(std::string_view(
                        reinterpret_cast<const char*>(bytecode.data), bytecode.size)),
                    .codeBytes = bytecode.size,
                    .flags = plan.framePlan.permutationKey,
                });
            }
            pipelineStates_.emplace(name, std::move(pso));
        }
#endif
        preparedPipelinePlanKey_ = pipelinePlanKey;
        return Status::ok();
    }


#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    struct TunedNativeDispatchCacheEntry {
        const char* dispatchName = nullptr;
        ID3D12PipelineState* pipelineState = nullptr;
    };

    Status rebuildTunedNativeDispatchCache(const Dx12Fsr4ReferenceBackendPlan& plan)
    {
        if (device_ == nullptr || !useCpuDp4aTunedRecording(plan)) {
            tunedNativeDispatchCache_.clear();
            tunedNativeDispatchCacheKey_ = std::numeric_limits<std::uint64_t>::max();
            return Status::ok();
        }
        const std::uint64_t key = reflectionValidationCacheKeyForPlan(plan.framePlan);
        if (tunedNativeDispatchCacheKey_ == key &&
            tunedNativeDispatchCache_.size() == plan.dispatchRecordPlan.size()) {
            return Status::ok();
        }
        tunedNativeDispatchCache_.clear();
        tunedNativeDispatchCache_.reserve(plan.dispatchRecordPlan.size());
        for (const Dx12Fsr4ReferenceDispatchRecordPlan& record : plan.dispatchRecordPlan) {
            const auto pso = pipelineStates_.find(record.pipelineCacheKey);
            if (pso == pipelineStates_.end() || pso->second.Get() == nullptr) {
                std::ostringstream message;
                message << "DX12 FSR4 CPU/DP4A tuned native dispatch cache is missing PSO for "
                        << record.pipelineCacheKey;
                return Status::invalidArgument(message.str());
            }
            tunedNativeDispatchCache_.push_back(TunedNativeDispatchCacheEntry{
                .dispatchName = record.dispatchName,
                .pipelineState = pso->second.Get(),
            });
        }
        tunedNativeDispatchCacheKey_ = key;
        return Status::ok();
    }
#endif

    bool useCpuDp4aTunedRecording(const Dx12Fsr4ReferenceBackendPlan& plan) const noexcept
    {
        return plan.framePlan.pipelineVariant == PipelineVariant::Dp4aTunedCandidate ||
               plan.framePlan.pipelineVariant == PipelineVariant::Dp4aOptimized;
    }

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    bool descriptorPatchUsesExternalBorrowedResource(const Dx12Fsr4ReferenceDescriptorPatchDesc& patch) const
    {
        return patch.externalBorrowed;
    }

    Status validateExternalBorrowedDescriptorViewCached(
        const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
        ID3D12Resource* nativeResource,
        const Dx12Fsr4ReferenceDescriptorSpace space,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        std::array<std::uint8_t, kDx12Fsr4ReferenceResourceLookupCount>& frameValidatedSpaces)
    {
        const std::size_t slot = resourceLookupIndex(resource.id);
        const std::uint8_t spaceBit = space == Dx12Fsr4ReferenceDescriptorSpace::Uav ? 0x2u : 0x1u;
        if ((frameValidatedSpaces[slot] & spaceBit) != 0u) {
            return Status::ok();
        }
        frameValidatedSpaces[slot] |= spaceBit;

        if (validatedExternalDescriptorResourceById_[slot] != nativeResource) {
            validatedExternalDescriptorResourceById_[slot] = nativeResource;
            validatedExternalDescriptorSpacesById_[slot] = 0u;
        }
        if ((validatedExternalDescriptorSpacesById_[slot] & spaceBit) != 0u) {
            return Status::ok();
        }

        const Status status = validateNativeDescriptorView(resource, nativeResource, space, frameDesc);
        if (!status.succeeded()) {
            return status;
        }
        validatedExternalDescriptorSpacesById_[slot] |= spaceBit;
        return Status::ok();
    }

    Status rebuildTunedDescriptorTemplate(const Dx12Fsr4ReferenceBackendPlan& plan)
    {
        const std::uint32_t descriptorsPerFrame = plan.descriptorRealizationPlan.shaderVisibleDescriptorCount;
        if (descriptorsPerFrame == 0 || device_ == nullptr) {
            descriptorTemplateHeap_.Reset();
            descriptorTemplateDescriptorsPerFrame_ = 0;
            descriptorTemplateResourceGeneration_ = std::numeric_limits<std::uint64_t>::max();
            descriptorTemplatePlanKey_ = std::numeric_limits<std::uint64_t>::max();
            return Status::ok();
        }
        const std::uint64_t descriptorPlanKey = descriptorTemplateCacheKeyForPlan(plan.framePlan);
        if (descriptorTemplateHeap_ != nullptr &&
            descriptorTemplateDescriptorsPerFrame_ == descriptorsPerFrame &&
            descriptorTemplateResourceGeneration_ == resourceGeneration_ &&
            descriptorTemplatePlanKey_ == descriptorPlanKey) {
            return Status::ok();
        }

        const Status heapStatus = createCpuOnlyDescriptorHeap(device_.Get(), descriptorsPerFrame, descriptorTemplateHeap_);
        if (!heapStatus.succeeded()) {
            return heapStatus;
        }
        descriptorTemplateDescriptorsPerFrame_ = descriptorsPerFrame;
        descriptorTemplateResourceGeneration_ = resourceGeneration_;
        descriptorTemplatePlanKey_ = descriptorPlanKey;

        const std::uint32_t perDispatchDescriptorStride = rootSignaturePlan_.descriptorHeap.totalCbvSrvUavDescriptors;
        for (std::uint32_t dispatchBase = 0; dispatchBase < descriptorsPerFrame; dispatchBase += perDispatchDescriptorStride) {
            initializeNullDescriptors(
                device_.Get(),
                descriptorTemplateHeap_.Get(),
                descriptorSize_,
                rootSignaturePlan_,
                dispatchBase);
        }

        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (patch.space != Dx12Fsr4ReferenceDescriptorSpace::Srv &&
                patch.space != Dx12Fsr4ReferenceDescriptorSpace::Uav) {
                continue;
            }
            if (patch.shaderVisibleHeapOffset == std::numeric_limits<std::uint32_t>::max()) {
                return Status::invalidArgument("DX12 FSR4 CPU/DP4A tuned descriptor template patch has no heap offset");
            }
            const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(patch.resolvedResource);
            if (resource == nullptr || !resource->validHandle) {
                if (patch.optional) {
                    continue;
                }
                std::ostringstream message;
                message << "DX12 FSR4 CPU/DP4A tuned descriptor template missing required resource "
                        << resourceIdName(patch.resolvedResource) << " for "
                        << (patch.dispatchName != nullptr ? patch.dispatchName : "unnamed dispatch");
                return Status::invalidArgument(message.str());
            }
            if (patch.externalBorrowed) {
                // Borrowed game resources can change per frame. The frame copy
                // starts from the cached null/internal template, then patches
                // these external descriptors directly into the active ring slot.
                continue;
            }
            ID3D12Resource* nativeResource = nativeResourceForRealizedResource(*resource, Dx12Fsr4ReferenceFrameDesc{});
            if (nativeResource == nullptr) {
                if (patch.optional) {
                    continue;
                }
                std::ostringstream message;
                message << "DX12 FSR4 CPU/DP4A tuned descriptor template native resource is missing for "
                        << resourceIdName(patch.resolvedResource);
                return Status::invalidArgument(message.str());
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuDescriptorHandleAt(
                descriptorTemplateHeap_.Get(),
                descriptorSize_,
                patch.shaderVisibleHeapOffset);
            if (patch.space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
                const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = srvDescForResource(*resource, nativeResource, Dx12Fsr4ReferenceFrameDesc{});
                device_->CreateShaderResourceView(nativeResource, &srvDesc, handle);
            } else {
                const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = uavDescForResource(*resource, nativeResource, Dx12Fsr4ReferenceFrameDesc{});
                device_->CreateUnorderedAccessView(nativeResource, nullptr, &uavDesc, handle);
            }
        }
        return Status::ok();
    }

    Status copyTunedDescriptorTemplateToCurrentRing(const Dx12Fsr4ReferenceBackendPlan& plan)
    {
        const std::uint32_t descriptorsPerFrame = plan.descriptorRealizationPlan.shaderVisibleDescriptorCount;
        if (descriptorsPerFrame == 0 || descriptorTemplateHeap_ == nullptr || descriptorHeap_ == nullptr) {
            return Status::ok();
        }
        device_->CopyDescriptorsSimple(
            descriptorsPerFrame,
            cpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_),
            cpuDescriptorHandleAt(descriptorTemplateHeap_.Get(), descriptorSize_, 0),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return Status::ok();
    }
#endif

    Status prepareDescriptors(const Dx12Fsr4ReferenceBackendPlan& plan, const Dx12Fsr4ReferenceFrameDesc& frameDesc)
    {
        if (plan.descriptorRealizationPlan.missingRequiredResourceDescriptorCount != 0 && frameDesc.commandList.value != nullptr) {
            std::ostringstream message;
            message << "DX12 FSR4 reference descriptor patching is missing "
                    << plan.descriptorRealizationPlan.missingRequiredResourceDescriptorCount
                    << " required resource descriptor(s)";
            return Status::invalidArgument(message.str());
        }

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        if (device_ == nullptr) {
            return Status::ok();
        }
        const std::uint32_t descriptorsPerFrame = plan.descriptorRealizationPlan.shaderVisibleDescriptorCount;
        const std::uint32_t desiredRingSlots = descriptorRingSlotCountForLayout(descriptorsPerFrame);
        if (descriptorHeap_ == nullptr || descriptorRingSlotCount_ != desiredRingSlots || descriptorRingDescriptorsPerFrame_ != descriptorsPerFrame) {
            const std::uint32_t descriptorCount = descriptorsPerFrame * desiredRingSlots;
            const Status heapStatus = createShaderVisibleDescriptorHeap(
                device_.Get(),
                descriptorCount,
                descriptorHeap_);
            if (!heapStatus.succeeded()) {
                return heapStatus;
            }
            descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            descriptorRingSlotCount_ = desiredRingSlots;
            descriptorRingDescriptorsPerFrame_ = descriptorsPerFrame;
            descriptorRingFrameIndex_ = 0;
            currentDescriptorHeapBaseOffset_ = 0;
            const std::uint32_t perDispatchDescriptorStride = rootSignaturePlan_.descriptorHeap.totalCbvSrvUavDescriptors;
            for (std::uint32_t slot = 0; slot < descriptorRingSlotCount_; ++slot) {
                const std::uint32_t frameBase = descriptorFrameBaseOffset(descriptorsPerFrame, slot);
                for (std::uint32_t dispatchBase = 0; dispatchBase < descriptorsPerFrame; dispatchBase += perDispatchDescriptorStride) {
                    initializeNullDescriptors(
                        device_.Get(),
                        descriptorHeap_.Get(),
                        descriptorSize_,
                        rootSignaturePlan_,
                        frameBase + dispatchBase);
                }
            }
        }
        if (descriptorHeap_ == nullptr || descriptorRingSlotCount_ == 0) {
            return Status::ok();
        }
        const std::uint32_t ringSlot = static_cast<std::uint32_t>(descriptorRingFrameIndex_++ % descriptorRingSlotCount_);
        currentDescriptorHeapBaseOffset_ = descriptorFrameBaseOffset(descriptorsPerFrame, ringSlot);

        const bool tunedDescriptors = useCpuDp4aTunedRecording(plan);
        if (tunedDescriptors) {
            const Status templateStatus = rebuildTunedDescriptorTemplate(plan);
            if (!templateStatus.succeeded()) {
                return templateStatus;
            }
            const Status copyStatus = copyTunedDescriptorTemplateToCurrentRing(plan);
            if (!copyStatus.succeeded()) {
                return copyStatus;
            }
        }

        // External game resources are the one descriptor domain whose native
        // format/flags are outside APUSR's control. Validate each unique
        // resource/space pair once per frame and cache successful contracts
        // across frames while the resource pointer stays stable. This is not
        // optional debug scaffolding: CreateShaderResourceView/
        // CreateUnorderedAccessView do not return HRESULT, so a bad borrowed
        // view can poison the device and only become visible on a later call.
        std::array<std::uint8_t, kDx12Fsr4ReferenceResourceLookupCount> frameValidatedExternalSpaces{};

        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (patch.space != Dx12Fsr4ReferenceDescriptorSpace::Srv &&
                patch.space != Dx12Fsr4ReferenceDescriptorSpace::Uav) {
                continue;
            }
            if (tunedDescriptors && !descriptorPatchUsesExternalBorrowedResource(patch)) {
                continue;
            }
            if (patch.shaderVisibleHeapOffset == std::numeric_limits<std::uint32_t>::max()) {
                return Status::invalidArgument("DX12 FSR4 reference descriptor patch has no shader-visible heap offset");
            }
            const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(patch.resolvedResource);
            if (resource == nullptr || !resource->validHandle) {
                if (patch.optional) {
                    continue;
                }
                std::ostringstream message;
                message << "DX12 FSR4 reference descriptor patch missing required resource "
                        << resourceIdName(patch.resolvedResource) << " for "
                        << (patch.dispatchName != nullptr ? patch.dispatchName : "unnamed dispatch");
                return Status::invalidArgument(message.str());
            }

            ID3D12Resource* nativeResource = nativeResourceForRealizedResource(*resource, frameDesc);
            if (nativeResource == nullptr) {
                if (patch.optional) {
                    continue;
                }
                std::ostringstream message;
                message << "DX12 FSR4 reference native resource is missing for " << resourceIdName(patch.resolvedResource);
                return Status::invalidArgument(message.str());
            }
            if (patch.externalBorrowed) {
                const Status viewStatus = validateExternalBorrowedDescriptorViewCached(
                    *resource,
                    nativeResource,
                    patch.space,
                    frameDesc,
                    frameValidatedExternalSpaces);
                if (!viewStatus.succeeded()) {
                    return viewStatus;
                }
            }
#if APUSR_ENABLE_HOT_PATH_GUARDS
            else {
                const Status viewStatus = validateNativeDescriptorView(*resource, nativeResource, patch.space, frameDesc);
                if (!viewStatus.succeeded()) {
                    return viewStatus;
                }
            }
#endif
            const D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuDescriptorHandleAt(
                descriptorHeap_.Get(),
                descriptorSize_,
                currentDescriptorHeapBaseOffset_ + patch.shaderVisibleHeapOffset);
            if (patch.space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
                const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = srvDescForResource(*resource, nativeResource, frameDesc);
                device_->CreateShaderResourceView(nativeResource, &srvDesc, handle);
            } else {
                const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = uavDescForResource(*resource, nativeResource, frameDesc);
                device_->CreateUnorderedAccessView(nativeResource, nullptr, &uavDesc, handle);
            }
        }
#else
        (void)frameDesc;
#endif
        return Status::ok();
    }

    Status prepareConstantUploads(const Dx12Fsr4ReferenceBackendPlan& plan)
    {
        constantUploadPlan_ = plan.constantUploadPlan;
        Pass0WeightsConstants passWeights{};
        if (constantUploadPlan_.hasPassWeights) {
            const Status weightsStatus = makeReferencePass0WeightsConstants(plan.framePlan.preset, passWeights);
            if (!weightsStatus.succeeded()) {
                return weightsStatus;
            }
        }

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        if (device_ == nullptr || constantUploadPlan_.perFrameByteSize == 0) {
            return Status::ok();
        }
        constexpr std::uint32_t kConstantUploadRingSlots = 4u * 50u;
        const std::uint64_t requiredBytes = constantUploadPlan_.perFrameByteSize * kConstantUploadRingSlots;
        if (constantUploadBuffer_ == nullptr || constantUploadBufferByteSize_ < requiredBytes) {
            unmapConstantUploadBuffer();
            const Status uploadStatus = createUploadBufferResource(
                device_.Get(),
                requiredBytes,
                "APUSR FSR4 reference constants upload ring",
                constantUploadBuffer_);
            if (!uploadStatus.succeeded()) {
                return uploadStatus;
            }
            constantUploadBufferByteSize_ = requiredBytes;
            constantUploadFrameIndex_ = 0;
        }

        const std::uint32_t ringSlot = constantUploadFrameIndex_++ % kConstantUploadRingSlots;
        currentConstantUploadBaseOffset_ = static_cast<std::uint64_t>(ringSlot) * constantUploadPlan_.perFrameByteSize;
        currentConstantUploadGpuBase_ = constantUploadBuffer_->GetGPUVirtualAddress() + currentConstantUploadBaseOffset_;

        const bool tunedConstants = useCpuDp4aTunedRecording(plan);
        void* mapped = nullptr;
        if (tunedConstants) {
            if (constantUploadMappedPtr_ == nullptr) {
                D3D12_RANGE readRange{};
                const HRESULT mapHr = constantUploadBuffer_->Map(0, &readRange, reinterpret_cast<void**>(&constantUploadMappedPtr_));
                if (FAILED(mapHr)) {
                    return hresultStatus("ID3D12Resource::Map(FSR4 CPU/DP4A tuned persistent constants upload ring)", mapHr);
                }
                constantUploadPersistentlyMapped_ = true;
            }
            mapped = constantUploadMappedPtr_;
        } else {
            if (constantUploadPersistentlyMapped_) {
                unmapConstantUploadBuffer();
            }
            D3D12_RANGE readRange{};
            const HRESULT mapHr = constantUploadBuffer_->Map(0, &readRange, &mapped);
            if (FAILED(mapHr)) {
                return hresultStatus("ID3D12Resource::Map(FSR4 reference constants upload ring)", mapHr);
            }
        }

        for (const Dx12Fsr4ReferenceConstantUploadSlot& slot : constantUploadPlan_.slots) {
            const void* src = nullptr;
            std::uint64_t size = slot.byteSize;
            switch (slot.role) {
            case ReferenceConstantBufferRole::Fsr4Upscaler:
                src = &plan.framePlan.constants;
                size = sizeof(plan.framePlan.constants);
                break;
            case ReferenceConstantBufferRole::SpdAutoExposure:
                src = &plan.framePlan.spdConstants;
                size = sizeof(plan.framePlan.spdConstants);
                break;
            case ReferenceConstantBufferRole::Rcas:
                src = &plan.framePlan.rcasConstants;
                size = sizeof(plan.framePlan.rcasConstants);
                break;
            case ReferenceConstantBufferRole::PassWeights:
                src = &passWeights;
                size = sizeof(passWeights);
                break;
            case ReferenceConstantBufferRole::None:
                break;
            }
            if (src == nullptr || size == 0) {
                continue;
            }
            std::memcpy(
                static_cast<std::uint8_t*>(mapped) + currentConstantUploadBaseOffset_ + slot.byteOffset,
                src,
                static_cast<std::size_t>(size));
        }
        if (!tunedConstants) {
            D3D12_RANGE writtenRange{};
            writtenRange.Begin = static_cast<SIZE_T>(currentConstantUploadBaseOffset_);
            writtenRange.End = static_cast<SIZE_T>(currentConstantUploadBaseOffset_ + constantUploadPlan_.perFrameByteSize);
            constantUploadBuffer_->Unmap(0, &writtenRange);
        }
#endif
        return Status::ok();
    }

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    const Dx12Fsr4ReferenceConstantUploadSlot* constantSlotForRole(const ReferenceConstantBufferRole role) const
    {
        for (const Dx12Fsr4ReferenceConstantUploadSlot& slot : constantUploadPlan_.slots) {
            if (slot.role == role) {
                return &slot;
            }
        }
        return nullptr;
    }

    struct ExternalResourceStateTable {
        std::array<D3D12_RESOURCE_STATES, kDx12Fsr4ReferenceResourceLookupCount> states{};
        std::array<bool, kDx12Fsr4ReferenceResourceLookupCount> valid{};

        [[nodiscard]] bool get(const ResourceId id, D3D12_RESOURCE_STATES& outState) const noexcept
        {
            const std::size_t slot = resourceLookupIndex(id);
            if (!valid[slot]) {
                return false;
            }
            outState = states[slot];
            return true;
        }

        void set(const ResourceId id, const D3D12_RESOURCE_STATES state) noexcept
        {
            const std::size_t slot = resourceLookupIndex(id);
            states[slot] = state;
            valid[slot] = true;
        }
    };

    D3D12_RESOURCE_STATES trackedStateForResource(
        const ResourceId id,
        const ExternalResourceStateTable& externalStates) const
    {
        const std::size_t slot = resourceLookupIndex(id);
        if (ownedResourceStateValidById_[slot]) {
            return ownedResourceStateById_[slot];
        }
        D3D12_RESOURCE_STATES externalState = D3D12_RESOURCE_STATE_COMMON;
        return externalStates.get(id, externalState) ? externalState : D3D12_RESOURCE_STATE_COMMON;
    }

    void updateTrackedStateForResource(
        const ResourceId id,
        const D3D12_RESOURCE_STATES state,
        ExternalResourceStateTable& externalStates)
    {
        const std::size_t slot = resourceLookupIndex(id);
        if (ownedNativeResourceById_[slot] != nullptr || ownedResourceStateValidById_[slot]) {
            ownedResourceStateById_[slot] = state;
            ownedResourceStateValidById_[slot] = true;
        } else {
            externalStates.set(id, state);
        }
    }

    Status transitionResource(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        const ResourceId id,
        const D3D12_RESOURCE_STATES afterState,
        ExternalResourceStateTable& externalStates)
    {
        if (commandList == nullptr || id == ResourceId::Null) {
            return Status::ok();
        }
        const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(id);
        if (resource == nullptr || resource->ownership == Dx12Fsr4ReferenceResourceOwnership::Alias) {
            return Status::ok();
        }
        ID3D12Resource* native = nativeResourceForRealizedResource(*resource, frameDesc);
        if (native == nullptr) {
            if (resource->optional) {
                return Status::ok();
            }
            std::ostringstream message;
            message << "DX12 FSR4 reference cannot transition missing resource " << resourceIdName(id);
            return Status::invalidArgument(message.str());
        }
        const D3D12_RESOURCE_STATES beforeState = trackedStateForResource(id, externalStates);
        if (beforeState == afterState) {
            if (afterState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.UAV.pResource = native;
                commandList->ResourceBarrier(1, &barrier);
            }
            return Status::ok();
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = native;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = beforeState;
        barrier.Transition.StateAfter = afterState;
        commandList->ResourceBarrier(1, &barrier);
        updateTrackedStateForResource(id, afterState, externalStates);
        return Status::ok();
    }

    Status recordPendingResourceInitialization(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        ExternalResourceStateTable& externalStates)
    {
        if (!pendingResourceInitialization_) {
            return Status::ok();
        }

        const auto initializerDst = ownedResources_.find(static_cast<std::uint32_t>(ResourceId::InitializerBuffer));
        const auto initializerSrc = ownedResources_.find(static_cast<std::uint32_t>(ResourceId::InitializerUploadBuffer));
        if (initializerDst != ownedResources_.end() && initializerSrc != ownedResources_.end() && !initializerBytes_.empty()) {
            const Status toCopy = transitionResource(
                commandList,
                frameDesc,
                ResourceId::InitializerBuffer,
                D3D12_RESOURCE_STATE_COPY_DEST,
                externalStates);
            if (!toCopy.succeeded()) {
                return toCopy;
            }
            const std::uint64_t copyBytes = std::min<std::uint64_t>(
                initializerDst->second->GetDesc().Width,
                static_cast<std::uint64_t>(initializerBytes_.size()));
            commandList->CopyBufferRegion(initializerDst->second.Get(), 0, initializerSrc->second.Get(), 0, copyBytes);
            const Status toSrv = transitionResource(
                commandList,
                frameDesc,
                ResourceId::InitializerBuffer,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                externalStates);
            if (!toSrv.succeeded()) {
                return toSrv;
            }
        }

        const auto defaultExposureDst = ownedResources_.find(static_cast<std::uint32_t>(ResourceId::InternalDefaultExposure));
        if (defaultExposureDst != ownedResources_.end() && defaultExposureUploadBuffer_ != nullptr) {
            const Status toCopy = transitionResource(
                commandList,
                frameDesc,
                ResourceId::InternalDefaultExposure,
                D3D12_RESOURCE_STATE_COPY_DEST,
                externalStates);
            if (!toCopy.succeeded()) {
                return toCopy;
            }
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = defaultExposureDst->second.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = defaultExposureUploadBuffer_.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = defaultExposureUploadFootprint_;
            commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            const Status toSrv = transitionResource(
                commandList,
                frameDesc,
                ResourceId::InternalDefaultExposure,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                externalStates);
            if (!toSrv.succeeded()) {
                return toSrv;
            }
        }

        pendingResourceInitialization_ = false;
        return Status::ok();
    }

    bool patchMatchesDispatch(const Dx12Fsr4ReferenceDescriptorPatchDesc& patch, const ReferenceDispatchDesc& dispatch) const
    {
        return patch.dispatchName != nullptr && dispatch.name != nullptr && std::strcmp(patch.dispatchName, dispatch.name) == 0;
    }

    Status transitionResourcesForDispatch(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        const ReferenceDispatchDesc& dispatch,
        ExternalResourceStateTable& externalStates)
    {
        struct DesiredResourceState {
            ResourceId id = ResourceId::Null;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        };
        std::array<DesiredResourceState, 64> desired{};
        std::size_t desiredCount = 0;
        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (!patchMatchesDispatch(patch, dispatch) ||
                (patch.space != Dx12Fsr4ReferenceDescriptorSpace::Srv && patch.space != Dx12Fsr4ReferenceDescriptorSpace::Uav)) {
                continue;
            }
            const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(patch.resolvedResource);
            if (resource == nullptr || !resource->validHandle) {
                if (patch.optional) {
                    continue;
                }
                std::ostringstream message;
                message << "DX12 FSR4 reference dispatch " << (dispatch.name != nullptr ? dispatch.name : "<unnamed>")
                        << " is missing required resource " << resourceIdName(patch.resolvedResource);
                return Status::invalidArgument(message.str());
            }
            const D3D12_RESOURCE_STATES state = desiredStateForDescriptorSpace(patch.space);
            bool merged = false;
            for (std::size_t i = 0; i < desiredCount; ++i) {
                if (desired[i].id == patch.resolvedResource) {
                    if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                        desired[i].state = state;
                    }
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (desiredCount >= desired.size()) {
                    return Status::runtimeFailure("DX12 FSR4 reference dispatch exceeded stack transition plan capacity");
                }
                desired[desiredCount++] = DesiredResourceState{patch.resolvedResource, state};
            }
        }
        for (std::size_t i = 0; i < desiredCount; ++i) {
            const Status status = transitionResource(
                commandList,
                frameDesc,
                desired[i].id,
                desired[i].state,
                externalStates);
            if (!status.succeeded()) {
                return status;
            }
        }
        return Status::ok();
    }

    Status transitionResourcesForDispatchTuned(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        const ReferenceDispatchDesc& dispatch,
        const Dx12Fsr4ReferenceDispatchRecordPlan& record,
        ExternalResourceStateTable& externalStates)
    {
        for (const Dx12Fsr4ReferenceDispatchResourceTransitionDesc& transition : record.resourceTransitions) {
            const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(transition.resource);
            if (resource == nullptr || !resource->validHandle) {
                if (transition.optional) {
                    continue;
                }
                std::ostringstream message;
                message << "DX12 FSR4 CPU/DP4A tuned dispatch " << (dispatch.name != nullptr ? dispatch.name : "<unnamed>")
                        << " is missing required resource " << resourceIdName(transition.resource);
                return Status::invalidArgument(message.str());
            }
            const Status status = transitionResource(
                commandList,
                frameDesc,
                transition.resource,
                desiredStateForDescriptorSpace(transition.descriptorSpace),
                externalStates);
            if (!status.succeeded()) {
                return status;
            }
        }
        return Status::ok();
    }

    Status bindRootConstantsForDispatch(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceDispatchRecordPlan& record)
    {
        if (record.constantBuffers.empty()) {
            return Status::ok();
        }
        if (constantUploadBuffer_ == nullptr) {
            return Status::invalidArgument("DX12 FSR4 reference dispatch requires constants but the upload ring was not created");
        }
        const D3D12_GPU_VIRTUAL_ADDRESS base = currentConstantUploadGpuBase_;
        for (const Dx12Fsr4ReferenceDispatchCbvBinding& cbv : record.constantBuffers) {
            std::uint64_t byteOffset = cbv.byteOffset;
            if (byteOffset == std::numeric_limits<std::uint64_t>::max()) {
                const Dx12Fsr4ReferenceConstantUploadSlot* slot = constantSlotForRole(cbv.role);
                if (slot != nullptr) {
                    byteOffset = slot->byteOffset;
                }
            }
            if (byteOffset == std::numeric_limits<std::uint64_t>::max() ||
                cbv.rootParameterIndex == std::numeric_limits<std::uint32_t>::max()) {
                std::ostringstream message;
                message << "DX12 FSR4 reference missing root-CBV upload slot for " << referenceConstantBufferRoleName(cbv.role);
                return Status::invalidArgument(message.str());
            }
            commandList->SetComputeRootConstantBufferView(cbv.rootParameterIndex, base + byteOffset);
        }
        return Status::ok();
    }

    const Dx12Fsr4ReferenceDispatchRecordPlan* recordPlanForDispatch(
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const ReferenceDispatchDesc& dispatch) const
    {
        for (const Dx12Fsr4ReferenceDispatchRecordPlan& record : plan.dispatchRecordPlan) {
            if (record.dispatchName != nullptr && dispatch.name != nullptr && std::strcmp(record.dispatchName, dispatch.name) == 0) {
                return &record;
            }
        }
        return nullptr;
    }

    std::uint32_t descriptorTableBaseForDispatch(
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const ReferenceDispatchDesc& dispatch,
        const Dx12Fsr4ReferenceDescriptorSpace space) const
    {
        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (!patchMatchesDispatch(patch, dispatch) || patch.space != space) {
                continue;
            }
            if (patch.shaderVisibleHeapOffset == std::numeric_limits<std::uint32_t>::max() ||
                patch.descriptorTableOffset == std::numeric_limits<std::uint32_t>::max()) {
                return std::numeric_limits<std::uint32_t>::max();
            }
            return patch.shaderVisibleHeapOffset - patch.descriptorTableOffset;
        }
        return std::numeric_limits<std::uint32_t>::max();
    }

    void bindDescriptorTablesForDispatch(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const ReferenceDispatchDesc& dispatch)
    {
        if (descriptorHeap_ == nullptr) {
            return;
        }
        if (rootSignaturePlan_.uavTableRootParameter != std::numeric_limits<std::uint32_t>::max()) {
            const std::uint32_t base = descriptorTableBaseForDispatch(plan, dispatch, Dx12Fsr4ReferenceDescriptorSpace::Uav);
            if (base != std::numeric_limits<std::uint32_t>::max()) {
                commandList->SetComputeRootDescriptorTable(
                    rootSignaturePlan_.uavTableRootParameter,
                    gpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + base));
            }
        }
        if (rootSignaturePlan_.srvTableRootParameter != std::numeric_limits<std::uint32_t>::max()) {
            const std::uint32_t base = descriptorTableBaseForDispatch(plan, dispatch, Dx12Fsr4ReferenceDescriptorSpace::Srv);
            if (base != std::numeric_limits<std::uint32_t>::max()) {
                commandList->SetComputeRootDescriptorTable(
                    rootSignaturePlan_.srvTableRootParameter,
                    gpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + base));
            }
        }
    }

    void bindDescriptorTablesForDispatchTuned(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceDispatchRecordPlan& record)
    {
        if (descriptorHeap_ == nullptr) {
            return;
        }
        if (rootSignaturePlan_.uavTableRootParameter != std::numeric_limits<std::uint32_t>::max() &&
            record.uavDescriptorTableBase != std::numeric_limits<std::uint32_t>::max()) {
            commandList->SetComputeRootDescriptorTable(
                rootSignaturePlan_.uavTableRootParameter,
                gpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + record.uavDescriptorTableBase));
        }
        if (rootSignaturePlan_.srvTableRootParameter != std::numeric_limits<std::uint32_t>::max() &&
            record.srvDescriptorTableBase != std::numeric_limits<std::uint32_t>::max()) {
            commandList->SetComputeRootDescriptorTable(
                rootSignaturePlan_.srvTableRootParameter,
                gpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + record.srvDescriptorTableBase));
        }
    }

    Status recordUavBarriersForDispatch(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        const ReferenceDispatchDesc& dispatch)
    {
        std::array<ID3D12Resource*, 32> writtenResources{};
        std::size_t writtenCount = 0;
        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (!patchMatchesDispatch(patch, dispatch) || !descriptorPatchWritesResource(patch)) {
                continue;
            }
            const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(patch.resolvedResource);
            if (resource == nullptr || !resource->validHandle) {
                continue;
            }
            ID3D12Resource* native = nativeResourceForRealizedResource(*resource, frameDesc);
            if (native == nullptr) {
                continue;
            }
            bool found = false;
            for (std::size_t i = 0; i < writtenCount; ++i) {
                if (writtenResources[i] == native) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (writtenCount >= writtenResources.size()) {
                    return Status::runtimeFailure("DX12 FSR4 reference dispatch exceeded stack UAV barrier capacity");
                }
                writtenResources[writtenCount++] = native;
            }
        }
        if (writtenCount == 0) {
            return Status::ok();
        }
        std::array<D3D12_RESOURCE_BARRIER, 32> barriers{};
        for (std::size_t i = 0; i < writtenCount; ++i) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.UAV.pResource = writtenResources[i];
            barriers[i] = barrier;
        }
        commandList->ResourceBarrier(static_cast<UINT>(writtenCount), barriers.data());
        return Status::ok();
    }

    Status clearSpdAtomicCounterForDispatch(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        const ReferenceDispatchDesc& dispatch,
        ExternalResourceStateTable& externalStates)
    {
        if (dispatch.kind != PassKind::SpdAutoExposure) {
            return Status::ok();
        }
        const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(ResourceId::SpdAtomicCount);
        if (resource == nullptr || !resource->validHandle) {
            return Status::invalidArgument("DX12 FSR4 reference SPD dispatch requires the SPD atomic counter resource");
        }
        ID3D12Resource* native = nativeResourceForRealizedResource(*resource, frameDesc);
        if (native == nullptr || descriptorHeap_ == nullptr) {
            return Status::invalidArgument("DX12 FSR4 reference SPD atomic counter cannot be cleared without a resource and descriptor heap");
        }
        std::uint32_t heapOffset = std::numeric_limits<std::uint32_t>::max();
        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (patchMatchesDispatch(patch, dispatch) && patch.resolvedResource == ResourceId::SpdAtomicCount &&
                patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
                heapOffset = patch.shaderVisibleHeapOffset;
                break;
            }
        }
        if (heapOffset == std::numeric_limits<std::uint32_t>::max()) {
            return Status::invalidArgument("DX12 FSR4 reference SPD atomic counter UAV descriptor was not found");
        }
        const Status toUav = transitionResource(
            commandList,
            frameDesc,
            ResourceId::SpdAtomicCount,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            externalStates);
        if (!toUav.succeeded()) {
            return toUav;
        }
        const UINT clearValues[4]{0u, 0u, 0u, 0u};
        commandList->ClearUnorderedAccessViewUint(
            gpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + heapOffset),
            cpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + heapOffset),
            native,
            clearValues,
            0,
            nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = native;
        commandList->ResourceBarrier(1, &barrier);
        return Status::ok();
    }

    bool frameNeedsDebugInformationClear(const ReferenceFramePlan& framePlan) const
    {
        for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
            if (dispatch.enabled && dispatch.kind == PassKind::DebugView) {
                return true;
            }
        }
        return false;
    }

    Status clearDebugInformationForFrame(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        ExternalResourceStateTable& externalStates)
    {
        if (!frameNeedsDebugInformationClear(plan.framePlan)) {
            return Status::ok();
        }
        const Dx12Fsr4ReferenceRealizedResourceDesc* resource = realizedResourceForId(ResourceId::DebugInformation);
        if (resource == nullptr || !resource->validHandle) {
            return Status::invalidArgument("DX12 FSR4 reference debug visualization requires the debug-information resource");
        }
        ID3D12Resource* native = nativeResourceForRealizedResource(*resource, frameDesc);
        if (native == nullptr || descriptorHeap_ == nullptr) {
            return Status::invalidArgument("DX12 FSR4 reference debug-information clear requires a resource and descriptor heap");
        }
        std::uint32_t heapOffset = std::numeric_limits<std::uint32_t>::max();
        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (patch.resolvedResource == ResourceId::DebugInformation &&
                patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav &&
                patch.shaderVisibleHeapOffset != std::numeric_limits<std::uint32_t>::max()) {
                heapOffset = patch.shaderVisibleHeapOffset;
                break;
            }
        }
        if (heapOffset == std::numeric_limits<std::uint32_t>::max()) {
            return Status::invalidArgument("DX12 FSR4 reference debug-information UAV descriptor was not found");
        }
        const Status toUav = transitionResource(
            commandList,
            frameDesc,
            ResourceId::DebugInformation,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            externalStates);
        if (!toUav.succeeded()) {
            return toUav;
        }
        const UINT clearValues[4]{0u, 0u, 0u, 0u};
        commandList->ClearUnorderedAccessViewUint(
            gpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + heapOffset),
            cpuDescriptorHandleAt(descriptorHeap_.Get(), descriptorSize_, currentDescriptorHeapBaseOffset_ + heapOffset),
            native,
            clearValues,
            0,
            nullptr);
        return Status::ok();
    }

    Status restoreExternalResources(
        ID3D12GraphicsCommandList* commandList,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        ExternalResourceStateTable& externalStates,
        const ExternalResourceStateTable& originalExternalStates)
    {
        if (!frameDesc.externalResources.restoreExternalResourceStatesAfterDispatch) {
            return Status::ok();
        }
        for (std::size_t slot = 0; slot < originalExternalStates.valid.size(); ++slot) {
            if (!originalExternalStates.valid[slot]) {
                continue;
            }
            const Status status = transitionResource(
                commandList,
                frameDesc,
                static_cast<ResourceId>(slot),
                originalExternalStates.states[slot],
                externalStates);
            if (!status.succeeded()) {
                return status;
            }
        }
        return Status::ok();
    }

    Status recordNativeDispatches(
        const Dx12Fsr4ReferenceBackendPlan& plan,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc,
        const apusr::profile::SampleDecision& profileDecision,
        const std::uint64_t configurationHash,
        const std::uint64_t cpuFrameBegin,
        const std::uint64_t cpuRecordBegin,
        std::uint32_t& outGpuTimestampSlot)
    {
        outGpuTimestampSlot = std::numeric_limits<std::uint32_t>::max();
        const bool captureCpu = profileDecision.capture && apusr::profile::cpuEnabled();
        ID3D12GraphicsCommandList* commandList = static_cast<ID3D12GraphicsCommandList*>(frameDesc.commandList.value);
        if (commandList == nullptr) {
            return Status::invalidArgument("DX12 FSR4 reference native recording requires a non-null command list");
        }
        if (rootSignature_ == nullptr) {
            return Status::invalidArgument("DX12 FSR4 reference native recording requires a root signature");
        }
        if (!gpuTimestampProfilerDisabledAfterFailure_) {
            const Status gpuTimestampStatus = ensureGpuTimestampProfilerCapacity(plan);
            if (!gpuTimestampStatus.succeeded()) {
                // Profiling is observational. A query-heap/readback failure must not
                // suppress the real FSR4 dispatches or change game behavior. Record
                // the failure once for diagnosis, then continue this runtime without
                // GPU timestamps. A fresh feature/runtime gets another opportunity.
                apusr::profile::recordStatus(
                    profileStream_, frameDesc.runner.frame.frameIndex,
                    "gpu_timestamp_profiler.disabled", gpuTimestampStatus);
                resetGpuTimestampProfiler();
                gpuTimestampProfilerDisabledAfterFailure_ = true;
            }
        }

        ExternalResourceStateTable externalStates{};
        ExternalResourceStateTable originalExternalStates{};
        for (const Dx12Fsr4ReferenceRealizedResourceDesc& resource : plan.resourceRealizationPlan.resources) {
            if (resource.ownership != Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed || !resource.validHandle) {
                continue;
            }
            const D3D12_RESOURCE_STATES initialState = normalizeDx12ResourceState(externalNativeStateForResource(frameDesc, resource.id));
            externalStates.set(resource.id, initialState);
            originalExternalStates.set(resource.id, initialState);
        }

        commandList->SetComputeRootSignature(rootSignature_.Get());
        if (descriptorHeap_ != nullptr) {
            ID3D12DescriptorHeap* heaps[] = {descriptorHeap_.Get()};
            commandList->SetDescriptorHeaps(1, heaps);
        }

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
        Dx12GpuDebugEventScope gpuFrameMarker(commandList, "APUSR_FSR4_FRAME");
#endif

        const Status initStatus = recordPendingResourceInitialization(commandList, frameDesc, externalStates);
        if (!initStatus.succeeded()) {
            return initStatus;
        }
        const Status debugClearStatus = clearDebugInformationForFrame(commandList, plan, frameDesc, externalStates);
        if (!debugClearStatus.succeeded()) {
            return debugClearStatus;
        }

        std::uint32_t gpuTimestampSlot = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t gpuTimestampQueryCursor = 0;
        beginGpuTimestampFrame(
            commandList, plan, frameDesc, profileDecision, configurationHash, gpuTimestampSlot, gpuTimestampQueryCursor);
        outGpuTimestampSlot = gpuTimestampSlot;

        const bool useTunedRecording = useCpuDp4aTunedRecording(plan);
        std::size_t recordIndex = 0;
        for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
            if (!dispatch.enabled) {
                continue;
            }
            const std::uint64_t dispatchCpuBegin = captureCpu ? apusr::profile::cpuTick() : 0u;

#if APUSR_ENABLE_GPU_DEBUG_MARKERS
            Dx12GpuDebugEventScope gpuDispatchMarker(commandList, dispatch.name != nullptr ? dispatch.name : "fsr4_unnamed_dispatch");
#endif

            const Dx12Fsr4ReferenceDispatchRecordPlan* record = nullptr;
            if (useTunedRecording) {
                if (recordIndex >= plan.dispatchRecordPlan.size()) {
                    return Status::invalidArgument("DX12 FSR4 CPU/DP4A tuned record plan ended before dispatch list");
                }
                record = &plan.dispatchRecordPlan[recordIndex++];
#if APUSR_ENABLE_HOT_PATH_GUARDS
                if (!cstrEquals(record->dispatchName, dispatch.name)) {
                    std::ostringstream message;
                    message << "DX12 FSR4 CPU/DP4A tuned record plan order mismatch for "
                            << (dispatch.name != nullptr ? dispatch.name : "<unnamed>");
                    return Status::invalidArgument(message.str());
                }
#endif
            } else {
                record = recordPlanForDispatch(plan, dispatch);
                if (record == nullptr) {
                    std::ostringstream message;
                    message << "DX12 FSR4 reference record plan is missing for " << (dispatch.name != nullptr ? dispatch.name : "<unnamed>");
                    return Status::invalidArgument(message.str());
                }
            }

            ID3D12PipelineState* pipelineState = nullptr;
            std::string generatedPsoKey;
            if (useTunedRecording) {
                if (recordIndex == 0 || (recordIndex - 1u) >= tunedNativeDispatchCache_.size()) {
                    return Status::invalidArgument("DX12 FSR4 CPU/DP4A tuned native dispatch cache is not prepared");
                }
                const TunedNativeDispatchCacheEntry& native = tunedNativeDispatchCache_[recordIndex - 1u];
#if APUSR_ENABLE_HOT_PATH_GUARDS
                if (!cstrEquals(native.dispatchName, dispatch.name)) {
                    return Status::invalidArgument("DX12 FSR4 CPU/DP4A tuned native dispatch cache order mismatch");
                }
#endif
                pipelineState = native.pipelineState;
            } else {
                generatedPsoKey = pipelineCacheKeyForDispatch(plan.framePlan, dispatch);
                const auto pso = pipelineStates_.find(generatedPsoKey);
                if (pso != pipelineStates_.end()) {
                    pipelineState = pso->second.Get();
                }
            }
            if (pipelineState == nullptr) {
                std::ostringstream message;
                message << "DX12 FSR4 reference PSO is missing for "
                        << (useTunedRecording ? record->pipelineCacheKey : generatedPsoKey);
                return Status::invalidArgument(message.str());
            }

            markGpuTimestampDispatchBegin(commandList, gpuTimestampSlot, gpuTimestampQueryCursor, plan, dispatch);

            const Status transitionStatus = useTunedRecording
                ? transitionResourcesForDispatchTuned(commandList, frameDesc, dispatch, *record, externalStates)
                : transitionResourcesForDispatch(commandList, plan, frameDesc, dispatch, externalStates);
            if (!transitionStatus.succeeded()) {
                return transitionStatus;
            }
            const Status clearStatus = clearSpdAtomicCounterForDispatch(commandList, plan, frameDesc, dispatch, externalStates);
            if (!clearStatus.succeeded()) {
                return clearStatus;
            }
            commandList->SetPipelineState(pipelineState);
            if (useTunedRecording) {
                bindDescriptorTablesForDispatchTuned(commandList, *record);
            } else {
                bindDescriptorTablesForDispatch(commandList, plan, dispatch);
            }
            const Status cbvStatus = bindRootConstantsForDispatch(commandList, *record);
            if (!cbvStatus.succeeded()) {
                return cbvStatus;
            }
            markGpuTimestampDispatchBodyBegin(commandList, gpuTimestampSlot);
            commandList->Dispatch(dispatch.grid.groupsX, dispatch.grid.groupsY, dispatch.grid.groupsZ);
            markGpuTimestampDispatchEnd(commandList, gpuTimestampSlot);
            if (captureCpu) {
                apusr::profile::recordCpuInterval({
                    .stream = profileStream_,
                    .frameId = frameDesc.runner.frame.frameIndex,
                    .sampleSequence = profileDecision.sampleSequence,
                    .startCpuTick = dispatchCpuBegin,
                    .endCpuTick = apusr::profile::cpuTick(),
                    .name = dispatch.name != nullptr ? dispatch.name : "fsr4_unnamed_dispatch",
                    .flags = dispatch.logicalStage,
                });
            }
        }

        endGpuTimestampFrame(commandList, gpuTimestampSlot, gpuTimestampQueryCursor);
        const Status restoreStatus = restoreExternalResources(commandList, frameDesc, externalStates, originalExternalStates);
        if (profileDecision.capture && gpuTimestampSlot != std::numeric_limits<std::uint32_t>::max() &&
            gpuTimestampSlot < gpuTimestampFrames_.size()) {
            Dx12GpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[gpuTimestampSlot];
            const std::uint64_t cpuEnd = captureCpu ? apusr::profile::cpuTick() : 0u;
            metadata.cpuTotalNs = apusr::profile::durationNs(cpuFrameBegin, cpuEnd);
            metadata.cpuRecordNs = apusr::profile::durationNs(cpuRecordBegin, cpuEnd);
        }
        return restoreStatus;
    }

#endif

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    ID3D12Resource* nativeResourceForRealizedResource(
        const Dx12Fsr4ReferenceRealizedResourceDesc& resource,
        const Dx12Fsr4ReferenceFrameDesc& frameDesc) const
    {
        if (resource.ownership == Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed) {
            return static_cast<ID3D12Resource*>(dx12ExternalHandleForResource(frameDesc, resource.id).value);
        }
        if (ID3D12Resource* native = ownedNativeResourceForId(resource.id)) {
            return native;
        }
        const auto it = ownedResources_.find(static_cast<std::uint32_t>(resource.id));
        if (it != ownedResources_.end()) {
            return it->second.Get();
        }
        return nullptr;
    }
#endif

    Dx12Fsr4ReferenceCreateDesc createDesc_{};
    Dx12Fsr4ReferenceRootSignaturePlan rootSignaturePlan_{};
    std::uint32_t cachedSrvCount_ = 0;
    std::uint32_t cachedUavCount_ = 0;
    std::uint32_t cachedCbvCount_ = 0;
    std::uint32_t cachedSamplerCount_ = 0;
    std::uint32_t preparedShaderBlobCount_ = 0;
    Dx12Fsr4ReferenceShaderReflectionManifest reflectionManifest_{};
    std::uint64_t reflectionValidationCacheKey_ = std::numeric_limits<std::uint64_t>::max();
    Dx12Fsr4ReferenceConstantUploadPlan constantUploadPlan_{};
    std::uint64_t runtimePlanGeneration_ = 0;
    std::uint64_t preparedResourceCacheKey_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t realizedResourceLookupGeneration_ = 0;
    std::uint64_t resourceGeneration_ = 0;
    Preset initializerPreset_ = Preset::Balanced;
    bool initializerPresetValid_ = false;
    std::vector<std::uint8_t> initializerBytes_;
    std::array<float, 2> defaultExposureBytes_{1.0f, 1.0f};
    bool reflectionManifestLoaded_ = false;
    std::uint64_t modelResetKey_ = 0;
    bool modelResetKeyValid_ = false;
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    ComPtr<ID3D12Device> device_;
    ComPtr<IDXGIAdapter3> profileAdapter_;
    bool profileMemoryBudgetUnavailableLogged_ = false;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    std::uint32_t descriptorSize_ = 0;
    std::uint32_t descriptorRingSlotCount_ = 0;
    std::uint32_t descriptorRingDescriptorsPerFrame_ = 0;
    std::uint64_t descriptorRingFrameIndex_ = 0;
    std::uint32_t currentDescriptorHeapBaseOffset_ = 0;
    ComPtr<ID3D12Resource> constantUploadBuffer_;
    std::uint8_t* constantUploadMappedPtr_ = nullptr;
    bool constantUploadPersistentlyMapped_ = false;
    std::uint64_t constantUploadBufferByteSize_ = 0;
    std::uint64_t currentConstantUploadBaseOffset_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS currentConstantUploadGpuBase_ = 0;
    std::uint32_t constantUploadFrameIndex_ = 0;
    ComPtr<ID3D12DescriptorHeap> descriptorTemplateHeap_;
    std::uint32_t descriptorTemplateDescriptorsPerFrame_ = 0;
    std::uint64_t descriptorTemplateResourceGeneration_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t descriptorTemplatePlanKey_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t tunedNativeDispatchCacheKey_ = std::numeric_limits<std::uint64_t>::max();
    std::vector<TunedNativeDispatchCacheEntry> tunedNativeDispatchCache_;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> pipelineStates_;
    std::unordered_map<std::uint32_t, ComPtr<ID3D12Resource>> ownedResources_;
    std::array<const Dx12Fsr4ReferenceRealizedResourceDesc*, kDx12Fsr4ReferenceResourceLookupCount> realizedResourceById_{};
    std::array<ID3D12Resource*, kDx12Fsr4ReferenceResourceLookupCount> ownedNativeResourceById_{};
    std::array<D3D12_RESOURCE_STATES, kDx12Fsr4ReferenceResourceLookupCount> ownedResourceStateById_{};
    std::array<bool, kDx12Fsr4ReferenceResourceLookupCount> ownedResourceStateValidById_{};
    std::array<ID3D12Resource*, kDx12Fsr4ReferenceResourceLookupCount> validatedExternalDescriptorResourceById_{};
    std::array<std::uint8_t, kDx12Fsr4ReferenceResourceLookupCount> validatedExternalDescriptorSpacesById_{};
    ComPtr<ID3D12Resource> defaultExposureUploadBuffer_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT defaultExposureUploadFootprint_{};
    bool pendingResourceInitialization_ = false;
    ComPtr<ID3D12QueryHeap> gpuTimestampQueryHeap_;
    ComPtr<ID3D12Resource> gpuTimestampReadback_;
    std::vector<Dx12GpuTimestampFrameMetadata> gpuTimestampFrames_;
    std::uint32_t gpuTimestampRingSlots_ = 0;
    std::uint32_t gpuTimestampQueriesPerSlot_ = 0;
    std::uint64_t gpuTimestampReadbackBytes_ = 0;
    std::uint64_t gpuTimestampFrameSequence_ = 0;
    bool gpuTimestampProfilerDisabledAfterFailure_ = false;
    bool gpuTimestampCsvHeaderWritten_ = false;
    bool gpuTimestampCpuClockStarted_ = false;
    std::chrono::steady_clock::time_point gpuTimestampCpuStartTime_{};
#endif
    Dx12Fsr4ReferenceBackendPlan runtimePlanCache_{};
    Dx12RuntimePlanStructureKey runtimePlanCacheKey_{};
    bool runtimePlanCacheValid_ = false;
    std::uint64_t preparedPipelinePlanKey_ = std::numeric_limits<std::uint64_t>::max();
    apusr::profile::StreamHandle profileStream_{};
    apusr::profile::SampleGate profileSampleGate_{};
    std::uint64_t profileLastConfigurationHash_ = 0u;
#if APUSR_ENABLE_GPU_DEBUG_MARKERS
    std::unordered_map<void*, std::string> profiledExternalResourceNames_;
    bool gpuDebugAnnotationModeAttempted_ = false;
    Dx12GpuAnnotationModeStatus gpuDebugAnnotationModeStatus_ = Dx12GpuAnnotationModeStatus::NotRequested;
#if APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API
    HRESULT gpuDebugAnnotationQueryInterfaceHr_ = S_OK;
    HRESULT gpuDebugAnnotationFeatureCheckHr_ = S_OK;
#endif
#endif
    bool initialized_ = false;
};

} // namespace


Dx12Fsr4ReferenceResourceRealizationPlan makeDx12Fsr4ReferenceResourceRealizationPlan(
    const ResourcePlan& resourcePlan,
    const ReferenceFramePlan& framePlan,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc)
{
    Dx12Fsr4ReferenceResourceRealizationPlan plan{};
    plan.resources.reserve(resourcePlan.resources.size() + resourcePlan.aliases.size());

    for (const ResourceDesc& resource : resourcePlan.resources) {
        Dx12Fsr4ReferenceRealizedResourceDesc realized{};
        realized.id = resource.id;
        realized.resolvedResource = resource.id;
        realized.name = resource.name;
        realized.kind = resource.kind;
        realized.lifetime = resource.lifetime;
        realized.format = resource.format;
        realized.width = resource.width;
        realized.height = resource.height;
        realized.byteSize = resource.byteSize;
        realized.srv = resource.srv;
        realized.uav = resource.uav;
        realized.renderTargetCompatible = resource.renderTargetCompatible;
        realized.optional = resource.optional;
        realized.temporal = resource.temporal;
        realized.requiredForSelectedFrame = externalResourceRequiredForSelectedFrame(framePlan, frameDesc, resource);
        realized.initialContents = resource.initialContents;
        realized.resizePolicy = resource.resizePolicy;

        if (resource.lifetime == ResourceLifetime::External) {
            realized.ownership = Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed;
            realized.nativeResource = dx12ExternalHandleForResource(frameDesc, resource.id);
            realized.validHandle = realized.nativeResource.value != nullptr;
            ++plan.externalBorrowedCount;
            if (!realized.validHandle) {
                if (realized.requiredForSelectedFrame) {
                    ++plan.missingRequiredExternalCount;
                } else {
                    ++plan.missingOptionalExternalCount;
                }
            }
        } else if (resource.lifetime == ResourceLifetime::Upload) {
            realized.ownership = Dx12Fsr4ReferenceResourceOwnership::UploadOwned;
            realized.validHandle = true;
            ++plan.uploadOwnedCount;
            plan.uploadByteSize += resourceCommittedByteEstimate(resource);
        } else {
            realized.ownership = Dx12Fsr4ReferenceResourceOwnership::InternalOwned;
            realized.validHandle = true;
            ++plan.internalOwnedCount;
            plan.persistentByteSize += resourceCommittedByteEstimate(resource);
        }

        if (resource.initialContents == ResourceInitialContents::InitializerBytes) {
            plan.needsInitializerUpload = true;
        }
        if (resource.initialContents == ResourceInitialContents::DefaultExposureOneOne) {
            plan.needsDefaultExposureUpload = true;
        }
        plan.resources.push_back(realized);
    }

    const bool rcasEnabled = framePlan.constants.rcasEnabled != 0;
    for (const ResourceAliasDesc& alias : resourcePlan.aliases) {
        Dx12Fsr4ReferenceRealizedResourceDesc realized{};
        realized.id = alias.id;
        realized.resolvedResource = dx12Fsr4ReferenceResolveAlias(resourcePlan, alias.id, rcasEnabled);
        realized.name = alias.name;
        realized.ownership = Dx12Fsr4ReferenceResourceOwnership::Alias;
        realized.srv = alias.srv;
        realized.uav = alias.uav;
        realized.requiredForSelectedFrame = resourceUsedBySelectedFrame(framePlan, alias.id);
        realized.validHandle = findResource(resourcePlan, realized.resolvedResource) != nullptr;
        ++plan.aliasCount;
        plan.resources.push_back(realized);
    }
    return plan;
}

const char* dx12Fsr4ReferenceDescriptorSpaceName(const Dx12Fsr4ReferenceDescriptorSpace space)
{
    switch (space) {
    case Dx12Fsr4ReferenceDescriptorSpace::Srv:
        return "srv";
    case Dx12Fsr4ReferenceDescriptorSpace::Uav:
        return "uav";
    case Dx12Fsr4ReferenceDescriptorSpace::Cbv:
        return "cbv";
    case Dx12Fsr4ReferenceDescriptorSpace::Sampler:
        return "sampler";
    }
    return "unknown";
}

const char* dx12Fsr4ReferenceRootParameterKindName(const Dx12Fsr4ReferenceRootParameterKind kind)
{
    switch (kind) {
    case Dx12Fsr4ReferenceRootParameterKind::SrvDescriptorTable:
        return "srv-descriptor-table";
    case Dx12Fsr4ReferenceRootParameterKind::UavDescriptorTable:
        return "uav-descriptor-table";
    case Dx12Fsr4ReferenceRootParameterKind::RootCbv:
        return "root-cbv";
    }
    return "unknown";
}

const char* dx12Fsr4ReferenceShaderProviderKindName(const Dx12Fsr4ReferenceShaderProviderKind kind)
{
    switch (kind) {
    case Dx12Fsr4ReferenceShaderProviderKind::PackagedCache:
        return "packaged-cache";
    case Dx12Fsr4ReferenceShaderProviderKind::EmbeddedCache:
        return "embedded-cache";
    case Dx12Fsr4ReferenceShaderProviderKind::RuntimeCompile:
        return "runtime-compile";
    }
    return "unknown";
}

const char* dx12Fsr4ReferenceResourceOwnershipName(const Dx12Fsr4ReferenceResourceOwnership ownership)
{
    switch (ownership) {
    case Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed:
        return "external-borrowed";
    case Dx12Fsr4ReferenceResourceOwnership::InternalOwned:
        return "internal-owned";
    case Dx12Fsr4ReferenceResourceOwnership::UploadOwned:
        return "upload-owned";
    case Dx12Fsr4ReferenceResourceOwnership::Alias:
        return "alias";
    }
    return "unknown";
}

std::string dx12Fsr4ReferenceShaderCacheKey(
    const Preset preset,
    const FrameCapacity frameCapacity,
    const std::uint32_t permutationKey,
    const char* dispatchName)
{
    std::ostringstream key;
    key << presetArtifactName(preset) << '/' << frameCapacityName(frameCapacity)
        << "/p" << permutationKey << '/' << (dispatchName != nullptr ? dispatchName : "null");
    return key.str();
}

std::string dx12Fsr4ReferencePackagedShaderBlobPath(
    const std::string& cacheRoot,
    const Preset preset,
    const FrameCapacity frameCapacity,
    const std::uint32_t permutationKey,
    const char* dispatchName)
{
    if (cacheRoot.empty() || dispatchName == nullptr || *dispatchName == '\0') {
        return {};
    }
    std::filesystem::path path(cacheRoot);
    path /= presetArtifactName(preset);
    path /= frameCapacityName(frameCapacity);
    path /= "p" + std::to_string(permutationKey);
    path /= std::string(dispatchName) + ".dxil";
    return path.generic_string();
}

Dx12Fsr4ReferenceRootSignaturePlan makeDx12Fsr4ReferenceRootSignaturePlan(
    const std::vector<Dx12Fsr4ReferenceDescriptorBinding>& descriptorBindings,
    const std::uint32_t srvCount,
    const std::uint32_t uavCount)
{
    Dx12Fsr4ReferenceRootSignaturePlan plan{};

    // FFX creates UAV and SRV descriptor tables in this order.  Keep that
    // ordering stable so the later recorder can bind one shader-visible
    // CBV/SRV/UAV heap without per-pass root-signature churn.
    if (uavCount != 0) {
        plan.uavTableRootParameter = static_cast<std::uint32_t>(plan.rootParameters.size());
        plan.rootParameters.push_back(Dx12Fsr4ReferenceRootParameterDesc{
            .kind = Dx12Fsr4ReferenceRootParameterKind::UavDescriptorTable,
            .space = Dx12Fsr4ReferenceDescriptorSpace::Uav,
            .rootParameterIndex = plan.uavTableRootParameter,
            .baseShaderRegister = 0,
            .registerSpace = 0,
            .descriptorCount = uavCount,
        });
    }
    if (srvCount != 0) {
        plan.srvTableRootParameter = static_cast<std::uint32_t>(plan.rootParameters.size());
        plan.rootParameters.push_back(Dx12Fsr4ReferenceRootParameterDesc{
            .kind = Dx12Fsr4ReferenceRootParameterKind::SrvDescriptorTable,
            .space = Dx12Fsr4ReferenceDescriptorSpace::Srv,
            .rootParameterIndex = plan.srvTableRootParameter,
            .baseShaderRegister = 0,
            .registerSpace = 0,
            .descriptorCount = srvCount,
        });
    }

    // FSR4, SPD, and RCAS each declare their primary constants at b0 in
    // their own shader scopes.  A single superset root signature must therefore
    // have one b0 root parameter, not one parameter per semantic role; the
    // recorder binds the appropriate constant-buffer GPUVA before each dispatch.
    const std::uint32_t primaryConstantsRootParameter = addRootCbvRegisterIfPresent(
        plan, descriptorBindings, 0, ReferenceConstantBufferRole::Fsr4Upscaler);
    plan.fsr4ConstantsRootParameter = primaryConstantsRootParameter;
    plan.spdConstantsRootParameter = primaryConstantsRootParameter;
    plan.rcasConstantsRootParameter = primaryConstantsRootParameter;

    plan.passWeightsRootParameter = addRootCbvRegisterIfPresent(
        plan, descriptorBindings, 1, ReferenceConstantBufferRole::PassWeights);

    bool needsHistorySampler = false;
    for (const Dx12Fsr4ReferenceDescriptorBinding& binding : descriptorBindings) {
        if (binding.space == Dx12Fsr4ReferenceDescriptorSpace::Sampler && binding.registerIndex == 0) {
            needsHistorySampler = true;
            break;
        }
    }
    if (needsHistorySampler) {
        plan.staticSamplers.push_back(Dx12Fsr4ReferenceStaticSamplerDesc{
            .shaderRegister = 0,
            .registerSpace = 0,
            .shaderName = "g_history_sampler",
            .filter = "linear",
            .addressMode = "clamp",
        });
    }

    // The later recorder will allocate SRVs first, then UAVs, in one
    // shader-visible CBV/SRV/UAV heap.  CBVs are root descriptors and static
    // samplers are baked into the root signature, so neither consumes heap
    // descriptor slots here.
    plan.descriptorHeap.srvBaseOffset = 0;
    plan.descriptorHeap.srvCount = srvCount;
    plan.descriptorHeap.uavBaseOffset = srvCount;
    plan.descriptorHeap.uavCount = uavCount;
    plan.descriptorHeap.totalCbvSrvUavDescriptors = srvCount + uavCount;

    return plan;
}

ResourceId dx12Fsr4ReferenceResolveAlias(
    const ResourcePlan& resourcePlan,
    const ResourceId resource,
    const bool rcasEnabled)
{
    if (const ResourceAliasDesc* alias = findAlias(resourcePlan, resource)) {
        if (rcasEnabled && alias->rcasEnabledTarget != ResourceId::Null) {
            return alias->rcasEnabledTarget;
        }
        if (alias->defaultTarget != ResourceId::Null) {
            return alias->defaultTarget;
        }
    }
    return resource;
}

Status loadDx12Fsr4ReferenceShaderReflectionManifest(
    const std::string& path,
    Dx12Fsr4ReferenceShaderReflectionManifest& outManifest)
{
    outManifest = Dx12Fsr4ReferenceShaderReflectionManifest{};
    if (path.empty()) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest path is empty");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest could not be opened: " + path);
    }
    std::string text;
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size <= 0) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest is empty: " + path);
    }
    text.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(text.data(), size);
    if (!stream) {
        return Status::runtimeFailure("DX12 FSR4 reference reflection manifest could not be read: " + path);
    }

    JsonValue root{};
    JsonParser parser(text);
    if (!parser.parse(root)) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest JSON parse failed: " + parser.error());
    }
    if (root.type != JsonType::Object) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest root must be a JSON object");
    }

    std::string error;
    if (!jsonStringField(root, "schema", outManifest.schema, true, error)) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest: " + error);
    }
    if (outManifest.schema != "apusr.fsr4_reference.shader_resource_manifest.v2" &&
        outManifest.schema != "apusr.fsr4_reference.shader_resource_manifest.v1" &&
        outManifest.schema != "apusr.fsr4_reference_dx12.reflection_manifest.v1") {
        return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest has unsupported schema: " + outManifest.schema);
    }
    if (!jsonStringField(root, "targetApi", outManifest.targetApi, false, error)) {
        return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest: " + error);
    }
    if (!outManifest.targetApi.empty() && outManifest.targetApi != "dx12") {
        return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest targets unsupported API: " + outManifest.targetApi);
    }
    if (!jsonStringField(root, "compiler", outManifest.compiler, false, error)) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest: " + error);
    }
    if (!jsonStringField(root, "targetProfile", outManifest.targetProfile, false, error)) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest: " + error);
    }

    std::unordered_map<std::string, std::vector<Dx12Fsr4ReferenceShaderReflectionManifestRegister>> resourceSets;
    if (const JsonValue* resourceSetsValue = jsonField(root, "resourceSets")) {
        if (resourceSetsValue->type != JsonType::Array) {
            return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest resourceSets must be an array");
        }
        resourceSets.reserve(resourceSetsValue->array.size());
        for (const JsonValue& resourceSetValue : resourceSetsValue->array) {
            if (resourceSetValue.type != JsonType::Object) {
                return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest resourceSets entries must be objects");
            }
            std::string resourceSetId;
            if (!jsonStringField(resourceSetValue, "id", resourceSetId, true, error)) {
                return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest resourceSet: " + error);
            }
            const JsonValue* resources = jsonField(resourceSetValue, "resources");
            if (resources == nullptr) {
                return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest resourceSet must contain resources: " + resourceSetId);
            }
            std::vector<Dx12Fsr4ReferenceShaderReflectionManifestRegister> registers;
            const Status parseStatus = parseDx12Fsr4ReferenceShaderManifestRegisters(*resources, resourceSetId, registers);
            if (!parseStatus.succeeded()) {
                return parseStatus;
            }
            if (!resourceSets.emplace(resourceSetId, std::move(registers)).second) {
                return Status::invalidArgument("DX12 FSR4 reference shader-resource manifest contains duplicate resourceSet id: " + resourceSetId);
            }
        }
    }

    const JsonValue* entries = jsonField(root, "entries");
    if (entries == nullptr || entries->type != JsonType::Array) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest must contain an entries array");
    }
    outManifest.entries.reserve(entries->array.size());
    for (const JsonValue& entryValue : entries->array) {
        if (entryValue.type != JsonType::Object) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest entries must be objects");
        }
        Dx12Fsr4ReferenceShaderReflectionManifestEntry entry{};
        std::string preset;
        std::string capacity;
        std::string passKind;
        if (!jsonStringField(entryValue, "preset", preset, true, error) ||
            !jsonStringField(entryValue, "capacity", capacity, true, error) ||
            !jsonStringField(entryValue, "dispatch", entry.dispatchName, true, error) ||
            !jsonStringField(entryValue, "passKind", passKind, true, error)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry: " + error);
        }
        if (!presetFromManifestName(preset, entry.preset)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry has unknown preset: " + preset);
        }
        if (!frameCapacityFromManifestName(capacity, entry.frameCapacity)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry has unknown capacity: " + capacity);
        }
        if (!passKindFromManifestName(passKind, entry.passKind)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry has unknown passKind: " + passKind);
        }
        std::uint32_t logicalStage = 0;
        if (!jsonUintField(entryValue, "logicalStage", logicalStage, false, error)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry: " + error);
        }
        entry.logicalStage = logicalStage;
        std::uint32_t permutationKey = 0;
        if (jsonField(entryValue, "permutationKey") != nullptr) {
            if (!jsonUintField(entryValue, "permutationKey", permutationKey, true, error)) {
                return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry: " + error);
            }
            entry.permutationKey = permutationKey;
            entry.hasPermutationKey = true;
        }
        if (!jsonStringField(entryValue, "entryPoint", entry.entryPoint, false, error) ||
            !jsonStringField(entryValue, "source", entry.source, false, error)) {
            return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry: " + error);
        }

        if (const JsonValue* resources = jsonField(entryValue, "resources")) {
            const Status parseStatus = parseDx12Fsr4ReferenceShaderManifestRegisters(*resources, entry.dispatchName, entry.registers);
            if (!parseStatus.succeeded()) {
                return parseStatus;
            }
        } else {
            std::string resourceSetId;
            if (!jsonStringField(entryValue, "resourceSetId", resourceSetId, true, error)) {
                return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry has neither resources nor resourceSetId: " + entry.dispatchName);
            }
            const auto resourceSetIt = resourceSets.find(resourceSetId);
            if (resourceSetIt == resourceSets.end()) {
                return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry references unknown resourceSetId " + resourceSetId + ": " + entry.dispatchName);
            }
            entry.registers = resourceSetIt->second;
        }
        std::uint32_t declaredResourceCount = 0;
        if (jsonField(entryValue, "resourceCount") != nullptr) {
            if (!jsonUintField(entryValue, "resourceCount", declaredResourceCount, true, error)) {
                return Status::invalidArgument("DX12 FSR4 reference reflection manifest entry: " + error);
            }
            if (declaredResourceCount != entry.registers.size()) {
                std::ostringstream message;
                message << "DX12 FSR4 reference reflection manifest entry resourceCount mismatch for "
                        << entry.dispatchName << ": declared " << declaredResourceCount
                        << " parsed " << entry.registers.size();
                return Status::invalidArgument(message.str());
            }
        }
        outManifest.registerCount += static_cast<std::uint32_t>(entry.registers.size());
        outManifest.entries.push_back(std::move(entry));
    }
    outManifest.entryCount = static_cast<std::uint32_t>(outManifest.entries.size());
    if (outManifest.entries.empty()) {
        return Status::invalidArgument("DX12 FSR4 reference reflection manifest contains no entries: " + path);
    }
    return Status::ok();
}

Dx12Fsr4ReferenceShaderReflectionManifestSelection selectDx12Fsr4ReferenceShaderReflectionManifestEntries(
    const Dx12Fsr4ReferenceShaderReflectionManifest& manifest,
    const ReferenceFramePlan& framePlan)
{
    Dx12Fsr4ReferenceShaderReflectionManifestSelection selection{};
    selection.manifestEntryCount = static_cast<std::uint32_t>(manifest.entries.size());
    selection.shaders.reserve(framePlan.dispatches.size());
    for (const Dx12Fsr4ReferenceShaderReflectionManifestEntry& entry : manifest.entries) {
        if (!manifestEntryMatchesFramePlan(entry, framePlan)) {
            ++selection.ignoredEntryCount;
            continue;
        }
        Dx12Fsr4ReferenceShaderReflectionDesc shader{};
        shader.dispatchName = entry.dispatchName.c_str();
        shader.passKind = entry.passKind;
        shader.logicalStage = entry.logicalStage;
        shader.registers.reserve(entry.registers.size());
        for (const Dx12Fsr4ReferenceShaderReflectionManifestRegister& reg : entry.registers) {
            shader.registers.push_back(Dx12Fsr4ReferenceShaderRegisterUse{
                .passKind = entry.passKind,
                .space = reg.space,
                .registerIndex = reg.registerIndex,
                .registerSpace = reg.registerSpace,
                .shaderName = reg.shaderName.empty() ? nullptr : reg.shaderName.c_str(),
                .required = reg.required,
            });
        }
        ++selection.selectedEntryCount;
        selection.shaders.push_back(std::move(shader));
    }
    return selection;
}

Status validateDx12Fsr4ReferenceShaderReflectionCoverage(
    const Dx12Fsr4ReferenceBackendPlan& plan,
    const std::vector<Dx12Fsr4ReferenceShaderReflectionDesc>& reflection,
    Dx12Fsr4ReferenceShaderReflectionValidationReport* outReport)
{
    Dx12Fsr4ReferenceShaderReflectionValidationReport report{};
    report.inspectedShaderCount = static_cast<std::uint32_t>(reflection.size());

    for (const Dx12Fsr4ReferenceShaderReflectionDesc& shader : reflection) {
        if (!framePlanHasDispatch(plan.framePlan, shader)) {
            ++report.missingManifestDispatchCount;
            std::ostringstream message;
            message << "reflection describes dispatch not present in frame plan: "
                    << (shader.dispatchName != nullptr ? shader.dispatchName : "<null>")
                    << " kind=" << passKindName(shader.passKind);
            report.errors.push_back(message.str());
        }

        for (const Dx12Fsr4ReferenceShaderRegisterUse& use : shader.registers) {
            if (!use.required) {
                continue;
            }
            ++report.inspectedRegisterCount;
            const bool rootCovered = rootSignatureCoversRegister(plan.rootSignature, use);
            if (!rootCovered) {
                ++report.missingRootSignatureCoverageCount;
                if (use.space == Dx12Fsr4ReferenceDescriptorSpace::Sampler) {
                    ++report.missingStaticSamplerCount;
                }
                std::ostringstream message;
                message << "root signature does not cover " << dx12Fsr4ReferenceDescriptorSpaceName(use.space)
                        << use.registerIndex << " space" << use.registerSpace
                        << " required by " << (shader.dispatchName != nullptr ? shader.dispatchName : "<null>");
                if (use.shaderName != nullptr) {
                    message << " (" << use.shaderName << ")";
                }
                report.errors.push_back(message.str());
            }

            const bool bindingCovered = bindingPlanCoversRegister(plan, shader, use);
            if (!bindingCovered) {
                ++report.missingBindingCount;
                std::ostringstream message;
                message << "descriptor binding plan does not cover " << dx12Fsr4ReferenceDescriptorSpaceName(use.space)
                        << use.registerIndex << " space" << use.registerSpace
                        << " required by " << (shader.dispatchName != nullptr ? shader.dispatchName : "<null>");
                if (use.shaderName != nullptr) {
                    message << " (" << use.shaderName << ")";
                }
                report.errors.push_back(message.str());
            }
        }
    }

    if (outReport != nullptr) {
        *outReport = report;
    }
    if (!report.errors.empty()) {
        std::ostringstream message;
        message << "DX12 FSR4 reference shader reflection validation failed: "
                << report.missingBindingCount << " missing binding(s), "
                << report.missingRootSignatureCoverageCount << " root-signature coverage miss(es)";
        return Status::invalidArgument(message.str());
    }
    return Status::ok();
}

Status validateDx12Fsr4ReferenceShaderReflectionManifestCoverage(
    const Dx12Fsr4ReferenceBackendPlan& plan,
    const Dx12Fsr4ReferenceShaderReflectionManifest& manifest,
    const bool requireCompleteDispatchCoverage,
    Dx12Fsr4ReferenceShaderReflectionValidationReport* outReport)
{
    Dx12Fsr4ReferenceShaderReflectionValidationReport report{};
    const Dx12Fsr4ReferenceShaderReflectionManifestSelection selection =
        selectDx12Fsr4ReferenceShaderReflectionManifestEntries(manifest, plan.framePlan);
    const Status coverageStatus = validateDx12Fsr4ReferenceShaderReflectionCoverage(plan, selection.shaders, &report);
    (void)coverageStatus;

    if (requireCompleteDispatchCoverage) {
        for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
            if (!dispatch.enabled) {
                continue;
            }
            bool found = false;
            for (const Dx12Fsr4ReferenceShaderReflectionDesc& shader : selection.shaders) {
                if (shader.passKind == dispatch.kind && shader.logicalStage == dispatch.logicalStage &&
                    cstrEquals(shader.dispatchName, dispatch.name)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ++report.missingManifestDispatchCount;
                std::ostringstream message;
                message << "reflection manifest does not contain selected dispatch "
                        << (dispatch.name != nullptr ? dispatch.name : "<null>")
                        << " kind=" << passKindName(dispatch.kind)
                        << " preset=" << presetName(plan.framePlan.preset)
                        << " capacity=" << frameCapacityName(plan.framePlan.frameCapacity)
                        << " permutationKey=" << plan.framePlan.permutationKey;
                report.errors.push_back(message.str());
            }
        }
    }

    if (outReport != nullptr) {
        *outReport = report;
    }
    if (!report.errors.empty()) {
        std::ostringstream message;
        message << "DX12 FSR4 reference shader reflection manifest validation failed: "
                << report.missingManifestDispatchCount << " missing manifest dispatch(es), "
                << report.missingBindingCount << " missing binding(s), "
                << report.missingRootSignatureCoverageCount << " root-signature coverage miss(es)";
        return Status::invalidArgument(message.str());
    }
    return Status::ok();
}

Status makeDx12Fsr4ReferenceBackendPlan(
    const Dx12Fsr4ReferenceCreateDesc& createDesc,
    const Dx12Fsr4ReferenceFrameDesc& frameDesc,
    Dx12Fsr4ReferenceBackendPlan& outPlan)
{
    outPlan = Dx12Fsr4ReferenceBackendPlan{};
    const Status inputStatus = validateDx12Fsr4ReferencePlanInputs(createDesc, frameDesc);
    if (!inputStatus.succeeded()) {
        return inputStatus;
    }

    outPlan.framePlan = makeReferenceFramePlan(createDesc.runner, frameDesc.runner);
    outPlan.resourcePlan = makeReferenceResourcePlan(ResourcePlanDesc{
        .maxRenderSize = createDesc.runner.maxRenderSize,
        .maxUpscaleSize = createDesc.runner.maxUpscaleSize,
        .includeExternalResources = true,
        .includeDebugInformation = createDesc.runner.allowDebugVisualization,
    });
    outPlan.shaders = makeDx12ShaderBuildPlan(outPlan.framePlan.dispatches);
    std::uint32_t frameSrvCount = 0;
    std::uint32_t frameUavCount = 0;
    std::uint32_t frameCbvCount = 0;
    std::uint32_t frameSamplerCount = 0;
    outPlan.descriptorBindings = makeDx12DescriptorBindings(
        outPlan.framePlan,
        frameSrvCount,
        frameUavCount,
        frameCbvCount,
        frameSamplerCount);
    outPlan.resourceUsagePlan = makeReferenceResourceUsagePlan(outPlan.framePlan);

    // The native DX12 runtime uses one root signature and one fixed descriptor
    // segment stride across frames. Derive that envelope from maximal legal
    // dispatch contracts, while keeping descriptorBindings itself frame-exact.
    const ReferenceFramePlan rootEnvelopePlan = makeDx12RootSignatureEnvelopeFramePlan(createDesc.runner);
    const std::vector<Dx12Fsr4ReferenceDescriptorBinding> rootSignatureBindings = makeDx12DescriptorBindings(
        rootEnvelopePlan,
        outPlan.srvCount,
        outPlan.uavCount,
        outPlan.cbvCount,
        outPlan.samplerCount);
    if (frameSrvCount > outPlan.srvCount || frameUavCount > outPlan.uavCount ||
        frameCbvCount > outPlan.cbvCount || frameSamplerCount > outPlan.samplerCount) {
        return Status::runtimeFailure("DX12 FSR4 exact dispatch bindings exceeded the runtime root-signature envelope");
    }
    outPlan.maxSrvRegister = outPlan.srvCount == 0 ? 0 : outPlan.srvCount - 1u;
    outPlan.maxUavRegister = outPlan.uavCount == 0 ? 0 : outPlan.uavCount - 1u;
    outPlan.maxCbvRegister = outPlan.cbvCount == 0 ? 0 : outPlan.cbvCount - 1u;
    outPlan.maxSamplerRegister = outPlan.samplerCount == 0 ? 0 : outPlan.samplerCount - 1u;
    outPlan.rootSignature = makeDx12Fsr4ReferenceRootSignaturePlan(
        rootSignatureBindings,
        outPlan.srvCount,
        outPlan.uavCount);
    outPlan.descriptorPatchPlan = makeDescriptorPatchPlan(
        outPlan.resourcePlan,
        outPlan.framePlan,
        outPlan.rootSignature);
    outPlan.dispatchRecordPlan = makeDispatchRecordPlan(
        outPlan.framePlan,
        outPlan.descriptorPatchPlan,
        outPlan.rootSignature);
    outPlan.resourceRealizationPlan = makeDx12Fsr4ReferenceResourceRealizationPlan(
        outPlan.resourcePlan,
        outPlan.framePlan,
        frameDesc);
    outPlan.descriptorRealizationPlan = makeDescriptorRealizationPlan(
        outPlan.rootSignature,
        outPlan.resourceRealizationPlan,
        outPlan.descriptorPatchPlan);
    outPlan.constantUploadPlan = makeConstantUploadPlan(
        outPlan.framePlan,
        outPlan.rootSignature);
    populateDispatchRecordConstantOffsets(outPlan.dispatchRecordPlan, outPlan.constantUploadPlan);

    outPlan.shaderProvider = makeShaderProviderPlan(createDesc.shaderProvider, outPlan.framePlan);
    outPlan.availableShaderBlobCount = outPlan.shaderProvider.totalResolvedShaderBlobCount;
    outPlan.missingShaderBlobCount = outPlan.shaderProvider.missingShaderBlobCount;

    // Keep backend-plan construction free of runtime filesystem reads.  The
    // real initializer bytes are loaded only when the runtime resource cache is
    // rebuilt, not while building every per-frame plan.
    outPlan.initializerBytes = outPlan.resourcePlan.initializerByteSize;

    if (outPlan.shaders.empty()) {
        return Status::invalidArgument("DX12 FSR4 reference frame produced no shader dispatches");
    }
    if (outPlan.descriptorBindings.empty()) {
        return Status::invalidArgument("DX12 FSR4 reference backend produced no descriptor bindings");
    }
    return Status::ok();
}

std::unique_ptr<Dx12Fsr4ReferenceRuntime> createDx12Fsr4ReferenceRuntime()
{
    return std::make_unique<PlannedDx12Fsr4ReferenceRuntime>();
}

} // namespace apusr::backends
