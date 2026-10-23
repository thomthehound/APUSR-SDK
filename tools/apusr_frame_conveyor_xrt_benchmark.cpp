// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/kernels/apusr_frame_conveyor_benchmark_abi.h>

#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_hw_context.h>
#include <xrt/xrt_kernel.h>
#if __has_include(<xrt/xrt_xclbin.h>)
#include <xrt/xrt_xclbin.h>
#elif __has_include(<xrt/experimental/xrt_xclbin.h>)
#include <xrt/experimental/xrt_xclbin.h>
#else
#error "XRT xclbin header not found"
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_XCLBIN
#define APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_XCLBIN ""
#endif

#ifndef APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_INSTS
#define APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_INSTS ""
#endif

namespace {

namespace bench = apusr::kernels::frame_conveyor_benchmark;

enum class BufferKind {
    HostOnly,
    Cacheable,
};

enum class BenchmarkStage {
    LoadOnly,
    AllocOnly,
    SyncOnly,
    Run,
};

struct Args {
    std::uint32_t deviceIndex = 0;
    std::uint32_t timeoutMs = 5000;
    std::uint32_t warmupFrames = 2;
    std::uint32_t frames = 30;
    std::uint64_t byteCount = bench::kConfiguredFrameBytes;
    BufferKind bufferKind = BufferKind::HostOnly;
    BenchmarkStage stage = BenchmarkStage::SyncOnly;
    bool allowUnsafeRun = false;
    std::filesystem::path xclbinPath;
    std::filesystem::path instsPath;
    std::string kernelName = "MLIR_AIE";
};

[[nodiscard]] bool parseU32(const char* text, std::uint32_t& out)
{
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (end == nullptr || *end != '\0' || value > 0xfffffffful) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool parseU64(const char* text, std::uint64_t& out)
{
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

[[nodiscard]] const char* bufferKindName(const BufferKind kind)
{
    switch (kind) {
    case BufferKind::HostOnly:
        return "host_only";
    case BufferKind::Cacheable:
        return "cacheable";
    default:
        return "unknown";
    }
}

[[nodiscard]] bool parseBufferKind(const char* text, BufferKind& out)
{
    const std::string_view value{text == nullptr ? "" : text};
    if (value == "host_only") {
        out = BufferKind::HostOnly;
        return true;
    }
    if (value == "cacheable") {
        out = BufferKind::Cacheable;
        return true;
    }
    return false;
}

[[nodiscard]] const char* stageName(const BenchmarkStage stage)
{
    switch (stage) {
    case BenchmarkStage::LoadOnly:
        return "load";
    case BenchmarkStage::AllocOnly:
        return "alloc";
    case BenchmarkStage::SyncOnly:
        return "sync";
    case BenchmarkStage::Run:
        return "run";
    default:
        return "unknown";
    }
}

[[nodiscard]] bool parseStage(const char* text, BenchmarkStage& out)
{
    const std::string_view value{text == nullptr ? "" : text};
    if (value == "load") {
        out = BenchmarkStage::LoadOnly;
        return true;
    }
    if (value == "alloc") {
        out = BenchmarkStage::AllocOnly;
        return true;
    }
    if (value == "sync") {
        out = BenchmarkStage::SyncOnly;
        return true;
    }
    if (value == "run") {
        out = BenchmarkStage::Run;
        return true;
    }
    return false;
}

[[nodiscard]] std::filesystem::path executableSibling(const char* argv0, const char* filename)
{
    if (filename == nullptr || filename[0] == '\0') {
        return {};
    }
    const std::filesystem::path executablePath{argv0 == nullptr ? "" : argv0};
    const std::filesystem::path directory = executablePath.has_parent_path() ? executablePath.parent_path() : std::filesystem::current_path();
    return directory / filename;
}

[[nodiscard]] std::vector<char> readFile(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        throw std::runtime_error("failed to query file size: " + path.string());
    }
    input.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(bytes.data(), size);
        if (!input) {
            throw std::runtime_error("failed to read file: " + path.string());
        }
    }
    return bytes;
}

void usage()
{
    std::cout
        << "apusr_frame_conveyor_xrt_benchmark [options]\n\n"
        << "Runs the standalone frame-conveyor DMA benchmark with XRT-owned BOs.\n"
        << "This is a CLI regression benchmark, not a game frontend path.\n\n"
        << "Options:\n"
        << "  --device <index>      XRT device index, default 0\n"
        << "  --bytes <n>           Payload bytes, default configured benchmark bytes\n"
        << "  --warmup <n>          Warmup frames before timing, default 2\n"
        << "  --frames <n>          Timed frames, default 30\n"
        << "  --timeout-ms <n>      XRT wait timeout per frame, default 5000\n"
        << "  --bo-kind <kind>      Payload BO flags: host_only or cacheable, default host_only\n"
        << "  --stage <stage>       Stop point: load, alloc, sync, or run; default sync\n"
        << "  --allow-unsafe-run    Required with --stage run; bad NPU kernels can hang/reset the machine\n"
        << "  --xclbin <path>       Override the benchmark xclbin copied beside this executable\n"
        << "  --insts <path>        Override the benchmark insts.bin copied beside this executable\n"
        << "  --kernel <name>       XRT kernel name, default MLIR_AIE\n"
        << "  --help                Show this message\n";
}

[[nodiscard]] bool parseArgs(const int argc, char** argv, Args& args)
{
    if (args.xclbinPath.empty()) {
        args.xclbinPath = executableSibling(argv[0], APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_XCLBIN);
    }
    if (args.instsPath.empty()) {
        args.instsPath = executableSibling(argv[0], APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_INSTS);
    }

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        auto needValue = [&](const char* name) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << "missing value for " << name << '\n';
                return nullptr;
            }
            return argv[++index];
        };

        if (arg == "--device") {
            const char* value = needValue("--device");
            if (value == nullptr || !parseU32(value, args.deviceIndex)) {
                std::cerr << "bad --device value\n";
                return false;
            }
        } else if (arg == "--bytes") {
            const char* value = needValue("--bytes");
            if (value == nullptr || !parseU64(value, args.byteCount)) {
                std::cerr << "bad --bytes value\n";
                return false;
            }
        } else if (arg == "--warmup") {
            const char* value = needValue("--warmup");
            if (value == nullptr || !parseU32(value, args.warmupFrames)) {
                std::cerr << "bad --warmup value\n";
                return false;
            }
        } else if (arg == "--frames") {
            const char* value = needValue("--frames");
            if (value == nullptr || !parseU32(value, args.frames)) {
                std::cerr << "bad --frames value\n";
                return false;
            }
        } else if (arg == "--timeout-ms") {
            const char* value = needValue("--timeout-ms");
            if (value == nullptr || !parseU32(value, args.timeoutMs)) {
                std::cerr << "bad --timeout-ms value\n";
                return false;
            }
        } else if (arg == "--bo-kind") {
            const char* value = needValue("--bo-kind");
            if (value == nullptr || !parseBufferKind(value, args.bufferKind)) {
                std::cerr << "bad --bo-kind value; expected host_only or cacheable\n";
                return false;
            }
        } else if (arg == "--stage") {
            const char* value = needValue("--stage");
            if (value == nullptr || !parseStage(value, args.stage)) {
                std::cerr << "bad --stage value; expected load, alloc, sync, or run\n";
                return false;
            }
        } else if (arg == "--allow-unsafe-run") {
            args.allowUnsafeRun = true;
        } else if (arg == "--xclbin") {
            const char* value = needValue("--xclbin");
            if (value == nullptr) {
                return false;
            }
            args.xclbinPath = value;
        } else if (arg == "--insts") {
            const char* value = needValue("--insts");
            if (value == nullptr) {
                return false;
            }
            args.instsPath = value;
        } else if (arg == "--kernel") {
            const char* value = needValue("--kernel");
            if (value == nullptr) {
                return false;
            }
            args.kernelName = value;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return false;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            usage();
            return false;
        }
    }

    if (args.stage == BenchmarkStage::Run && args.frames == 0) {
        std::cerr << "--frames must be non-zero for --stage run\n";
        return false;
    }
    if (args.stage == BenchmarkStage::Run && !args.allowUnsafeRun) {
        std::cerr << "--stage run requires --allow-unsafe-run; bad NPU kernels can hang/reset the machine\n";
        return false;
    }
    if (args.byteCount != bench::kConfiguredFrameBytes) {
        std::cerr << "--bytes must match the configured benchmark semantic byte count ("
                  << bench::kConfiguredFrameBytes
                  << "); reconfigure APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES to test another size\n";
        return false;
    }
    if (args.byteCount == 0) {
        std::cerr << "--bytes must be non-zero\n";
        return false;
    }
    if (bench::kConfiguredTransportBytes < bench::kConfiguredFrameBytes ||
        (bench::kConfiguredTransportBytes % bench::kChunkBytes) != 0) {
        std::cerr << "configured transport bytes must be a 4096-byte chunk multiple\n";
        return false;
    }
    if (bench::kColumnCount == 0 || bench::kColumnCount > 8) {
        std::cerr << "APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS must be in [1, 8]\n";
        return false;
    }
    if (bench::kDmaChannelsPerColumn == 0 || bench::kDmaChannelsPerColumn > 2) {
        std::cerr << "APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS must be 1 or 2\n";
        return false;
    }
    if (bench::kConfiguredFrameChunks < bench::kParallelDmaStreams) {
        std::cerr << "frame transport chunk count must be at least the parallel DMA stream count\n";
        return false;
    }
    if (args.xclbinPath.empty()) {
        std::cerr << "no default xclbin path was compiled in; pass --xclbin\n";
        return false;
    }
    if (args.instsPath.empty()) {
        std::cerr << "no default insts.bin path was compiled in; pass --insts\n";
        return false;
    }
    return true;
}

void fillInputPattern(std::uint8_t* data, const std::uint64_t byteCount, const std::uint32_t frameIndex)
{
    const std::uint32_t seed = 0x4e505553u ^ (frameIndex * 0x9e3779b9u);
    for (std::uint64_t index = 0; index < byteCount; ++index) {
        data[index] = static_cast<std::uint8_t>((seed + index * 131u + (index >> 5U)) & 0xffu);
    }
}

[[nodiscard]] std::uint64_t firstMismatch(const std::uint8_t* expected, const std::uint8_t* actual, const std::uint64_t byteCount)
{
    for (std::uint64_t index = 0; index < byteCount; ++index) {
        if (expected[index] != actual[index]) {
            return index;
        }
    }
    return byteCount;
}

[[nodiscard]] const char* commandStateName(const ert_cmd_state state)
{
    switch (state) {
    case ERT_CMD_STATE_NEW:
        return "NEW";
    case ERT_CMD_STATE_QUEUED:
        return "QUEUED";
    case ERT_CMD_STATE_RUNNING:
        return "RUNNING";
    case ERT_CMD_STATE_COMPLETED:
        return "COMPLETED";
    case ERT_CMD_STATE_ERROR:
        return "ERROR";
    case ERT_CMD_STATE_ABORT:
        return "ABORT";
    case ERT_CMD_STATE_SUBMITTED:
        return "SUBMITTED";
    case ERT_CMD_STATE_TIMEOUT:
        return "TIMEOUT";
    case ERT_CMD_STATE_NORESPONSE:
        return "NORESPONSE";
    case ERT_CMD_STATE_SKERROR:
        return "SKERROR";
    default:
        return "UNKNOWN";
    }
}

[[nodiscard]] double gbPerSecond(const std::uint64_t bytes, const std::int64_t microseconds)
{
    if (microseconds <= 0) {
        return 0.0;
    }
    return (static_cast<double>(bytes) * 1000000.0) / (static_cast<double>(microseconds) * 1000000000.0);
}

} // namespace

int main(const int argc, char** argv)
{
    Args args{};
    if (!parseArgs(argc, argv, args)) {
        return 2;
    }

    try {
        const std::vector<char> xclbinBytes = readFile(args.xclbinPath);
        const std::vector<char> instructionBytes = readFile(args.instsPath);
        if (xclbinBytes.empty()) {
            throw std::runtime_error("xclbin file is empty: " + args.xclbinPath.string());
        }
        if (instructionBytes.empty()) {
            throw std::runtime_error("insts.bin file is empty: " + args.instsPath.string());
        }
        if ((instructionBytes.size() % sizeof(std::uint32_t)) != 0) {
            throw std::runtime_error("insts.bin is not uint32-aligned");
        }

        std::cout << "Frame-conveyor XRT benchmark: stage=" << stageName(args.stage)
                  << " semantic_bytes=" << args.byteCount
                  << " transport_bytes=" << bench::kConfiguredTransportBytes
                  << " padding_bytes=" << bench::kConfiguredTransportPaddingBytes
                  << " columns=" << bench::kColumnCount
                  << " channels=" << bench::kDmaChannelsPerColumn
                  << " streams=" << bench::kParallelDmaStreams
                  << " chunks=" << bench::kConfiguredFrameChunks
                  << " base_chunks_per_stream=" << bench::kBaseChunksPerStream
                  << " extra_chunk_streams=" << bench::kExtraChunkStreams
                  << " bo_kind=" << bufferKindName(args.bufferKind)
                  << " xclbin=" << args.xclbinPath.string()
                  << " insts=" << args.instsPath.string()
                  << " xclbin_bytes=" << xclbinBytes.size()
                  << " inst_words=" << (instructionBytes.size() / sizeof(std::uint32_t)) << std::endl;

        xrt::device device{args.deviceIndex};
        xrt::xclbin xclbin{xclbinBytes};
        const xrt::uuid uuid = device.register_xclbin(xclbin);
        xrt::hw_context hwContext{device, uuid, xrt::hw_context::access_mode::shared};
        xrt::kernel kernel{hwContext, args.kernelName};

        const auto instructionGroup = static_cast<xrt::memory_group>(kernel.group_id(bench::kInstructionBufferArgIndex));
        const auto inputGroup = static_cast<xrt::memory_group>(kernel.group_id(bench::kInputFrameBufferArgIndex));
        const auto outputGroup = static_cast<xrt::memory_group>(kernel.group_id(bench::kOutputFrameBufferArgIndex));

        std::cout << "  loaded kernel name=" << args.kernelName
                  << " groups={inst:" << static_cast<int>(instructionGroup)
                  << ", input:" << static_cast<int>(inputGroup)
                  << ", output:" << static_cast<int>(outputGroup) << "}" << std::endl;
        if (args.stage == BenchmarkStage::LoadOnly) {
            std::cout << "PASS: load stage completed; no payload BOs allocated and no run submitted" << std::endl;
            return 0;
        }

        xrt::bo instructionBo{hwContext, instructionBytes.size(), xrt::bo::flags::cacheable, instructionGroup};
        std::memcpy(instructionBo.map<void*>(), instructionBytes.data(), instructionBytes.size());
        instructionBo.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        const xrt::bo::flags payloadFlags = args.bufferKind == BufferKind::Cacheable ? xrt::bo::flags::cacheable : xrt::bo::flags::host_only;
        xrt::bo inputBo{hwContext, static_cast<size_t>(bench::kConfiguredTransportBytes), payloadFlags, inputGroup};
        xrt::bo outputBo{hwContext, static_cast<size_t>(bench::kConfiguredTransportBytes), payloadFlags, outputGroup};

        auto* input = inputBo.map<std::uint8_t*>();
        auto* output = outputBo.map<std::uint8_t*>();
        std::cout << "  allocated and mapped payload BOs" << std::endl;
        if (args.stage == BenchmarkStage::AllocOnly) {
            std::cout << "PASS: alloc stage completed; no payload sync and no run submitted" << std::endl;
            return 0;
        }

        xrt::run run{kernel};
        run.set_arg(bench::kOpcodeArgIndex, bench::kKernelOpcode);
        run.set_arg(bench::kInstructionBufferArgIndex, instructionBo);
        run.set_arg(bench::kInstructionCountArgIndex, static_cast<std::uint32_t>(instructionBytes.size() / sizeof(std::uint32_t)));
        run.set_arg(bench::kInputFrameBufferArgIndex, inputBo);
        run.set_arg(bench::kOutputFrameBufferArgIndex, outputBo);

        fillInputPattern(input, bench::kConfiguredTransportBytes, 0);
        std::memset(output, 0xa5, static_cast<size_t>(bench::kConfiguredTransportBytes));
        inputBo.sync(XCL_BO_SYNC_BO_TO_DEVICE, static_cast<size_t>(bench::kConfiguredTransportBytes), 0);
        outputBo.sync(XCL_BO_SYNC_BO_TO_DEVICE, static_cast<size_t>(bench::kConfiguredTransportBytes), 0);
        std::cout << "  payload sync-to-device completed" << std::endl;
        if (args.stage == BenchmarkStage::SyncOnly) {
            std::cout << "PASS: sync stage completed; no run submitted" << std::endl;
            return 0;
        }

        std::cout << "  submitting NPU runs: warmup=" << args.warmupFrames
                  << " frames=" << args.frames << std::endl;

        std::vector<std::int64_t> elapsedUs;
        elapsedUs.reserve(args.frames);
        const std::uint32_t totalFrames = args.warmupFrames + args.frames;
        for (std::uint32_t frameIndex = 0; frameIndex < totalFrames; ++frameIndex) {
            fillInputPattern(input, bench::kConfiguredTransportBytes, frameIndex);
            std::memset(output, 0xa5, static_cast<size_t>(bench::kConfiguredTransportBytes));
            inputBo.sync(XCL_BO_SYNC_BO_TO_DEVICE, static_cast<size_t>(bench::kConfiguredTransportBytes), 0);
            outputBo.sync(XCL_BO_SYNC_BO_TO_DEVICE, static_cast<size_t>(bench::kConfiguredTransportBytes), 0);

            const auto begin = std::chrono::steady_clock::now();
            run.start();
            const std::cv_status status = run.wait2(std::chrono::milliseconds{args.timeoutMs});
            const auto end = std::chrono::steady_clock::now();
            if (status == std::cv_status::timeout) {
                std::cerr << "FAIL: XRT run timed out after " << args.timeoutMs << " ms; state="
                          << commandStateName(run.state()) << '(' << static_cast<int>(run.state()) << ")\n";
                return 3;
            }

            outputBo.sync(XCL_BO_SYNC_BO_FROM_DEVICE, static_cast<size_t>(bench::kConfiguredTransportBytes), 0);
            const std::uint64_t mismatch = firstMismatch(input, output, bench::kConfiguredTransportBytes);
            if (mismatch != bench::kConfiguredTransportBytes) {
                std::cerr << "FAIL: output mismatch on frame " << frameIndex << " at byte " << mismatch
                          << " expected=0x" << std::hex << static_cast<unsigned>(input[mismatch])
                          << " actual=0x" << static_cast<unsigned>(output[mismatch]) << std::dec << "\n";
                return 4;
            }

            if (frameIndex >= args.warmupFrames) {
                elapsedUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
            }
        }

        const auto [minIt, maxIt] = std::minmax_element(elapsedUs.begin(), elapsedUs.end());
        const std::int64_t sumUs = std::accumulate(elapsedUs.begin(), elapsedUs.end(), std::int64_t{0});
        const std::int64_t avgUs = sumUs / static_cast<std::int64_t>(elapsedUs.size());
        const std::uint64_t semanticRoundTripBytes = args.byteCount * 2u;
        const std::uint64_t transportRoundTripBytes = bench::kConfiguredTransportBytes * 2u;
        std::cout << "PASS: copied semantic=" << args.byteCount
                  << " transport=" << bench::kConfiguredTransportBytes
                  << " bytes/frame over " << elapsedUs.size() << " timed frames\n"
                  << "  min_us=" << *minIt << " avg_us=" << avgUs << " max_us=" << *maxIt << '\n'
                  << "  effective_GBps_semantic_one_way=" << gbPerSecond(args.byteCount, avgUs) << '\n'
                  << "  effective_GBps_semantic_roundtrip=" << gbPerSecond(semanticRoundTripBytes, avgUs) << '\n'
                  << "  effective_GBps_transport_one_way=" << gbPerSecond(bench::kConfiguredTransportBytes, avgUs) << '\n'
                  << "  effective_GBps_transport_roundtrip=" << gbPerSecond(transportRoundTripBytes, avgUs) << std::endl;
        return 0;
    } catch (const xrt::run::command_error& error) {
        const ert_cmd_state state = error.get_command_state();
        std::cerr << "FAIL: XRT command error: " << error.what()
                  << " command_state=" << commandStateName(state)
                  << '(' << static_cast<int>(state) << ")\n";
        return 5;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
