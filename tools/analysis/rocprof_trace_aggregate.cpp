// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
// Minimal ROCprof Trace Decoder aggregation CLI for APUSR analysis.
//
// It decodes an ATT/SQTT payload with rocprof-trace-decoder and aggregates
// instruction timing by (code_object_id, pc).  For RGP work it can also emit
// wave ranges, raw instruction records, realtime clock anchors, SQTT events,
// occupancy records, shaderdata, perf counters, and GFX11+ other-SIMD memory
// activity so Python tooling can correlate decoded SQTT slices with RGP
// QueueEvent timestamps.

#include <rocprof_trace_decoder/rocprof_trace_decoder.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct Options
{
    std::string input_path;
    std::string out_csv;
    std::string summary_json;
    std::string wave_csv;
    std::string raw_instruction_csv;
    std::string realtime_csv;
    std::string event_csv;
    std::string occupancy_csv;
    std::string shaderdata_csv;
    std::string perf_csv;
    std::string other_simd_csv;
    bool        keep_raw = false;
    int64_t     min_time = LLONG_MIN;
    int64_t     max_time = LLONG_MAX;
};

struct Key
{
    uint64_t code_object_id = 0;
    uint64_t pc_address     = 0;
    bool operator==(const Key& rhs) const { return code_object_id == rhs.code_object_id && pc_address == rhs.pc_address; }
};

struct KeyHash
{
    size_t operator()(const Key& k) const
    {
        return std::hash<uint64_t>{}(k.code_object_id ^ (k.pc_address * 0x9E3779B185EBCA87ull));
    }
};

struct Agg
{
    uint64_t hitcount = 0;
    int64_t latency   = 0;
    int64_t stall     = 0;
    int64_t active    = 0;
    int64_t idle      = 0;
    uint32_t category = 0;
};

struct WaveRow
{
    uint64_t wave_index = 0;
    uint32_t cu = 0, simd = 0, wave_id = 0, dispatcher = 0, workgroup_id = 0;
    int64_t begin_time = 0, end_time = 0;
    uint64_t instructions = 0;
};

struct RawInstRow
{
    uint64_t wave_index = 0, instruction_index = 0;
    uint32_t cu = 0, simd = 0, wave_id = 0, category = 0;
    int64_t time = 0, duration = 0, stall = 0, active = 0, idle = 0;
    uint64_t code_object_id = 0, pc_address = 0;
};

struct RealtimeRow
{
    int64_t shader_clock = 0;
    uint64_t realtime_clock = 0;
    uint64_t reserved = 0;
};

struct EventRow
{
    int64_t time = 0;
    uint32_t type = 0;
    uint32_t me_id = 0;
    uint32_t pipe_id = 0;
    uint32_t flags = 0;
    uint64_t payload = 0;
    uint64_t byte_offset = 0;
};

struct OccupancyRow
{
    uint64_t address = 0, code_object_id = 0, time = 0;
    uint32_t cu = 0, simd = 0, wave_id = 0, start = 0, me_id = 0, pipe_id = 0, is_ext = 0, workgroup_id = 0;
};

struct ShaderDataRow
{
    int64_t time = 0;
    uint64_t value = 0;
    uint32_t cu = 0, simd = 0, wave_id = 0, flags = 0, reserved = 0;
};

struct PerfRow
{
    int64_t time = 0;
    uint32_t events0 = 0, events1 = 0, events2 = 0, events3 = 0, cu = 0, bank = 0;
};

struct OtherSimdRow
{
    uint64_t size = 0;
    int64_t time = 0;
    uint32_t cycles = 0, wgp = 0, category = 0;
};

struct State
{
    const Options* options = nullptr;
    std::unordered_map<Key, Agg, KeyHash> by_pc;
    std::vector<WaveRow> waves_out;
    std::vector<RawInstRow> raw_out;
    std::vector<RealtimeRow> realtime_out;
    std::vector<EventRow> event_out;
    std::vector<OccupancyRow> occupancy_out;
    std::vector<ShaderDataRow> shaderdata_out;
    std::vector<PerfRow> perf_out;
    std::vector<OtherSimdRow> other_simd_out;
    uint64_t record_counts[64] = {};
    uint64_t waves             = 0;
    uint64_t instructions_seen = 0;
    uint64_t instructions_kept = 0;
    int64_t  min_wave_begin    = LLONG_MAX;
    int64_t  max_wave_end      = LLONG_MIN;
    int64_t  min_inst_time     = LLONG_MAX;
    int64_t  max_inst_end      = LLONG_MIN;
    int64_t  min_realtime_shader_clock = LLONG_MAX;
    int64_t  max_realtime_shader_clock = LLONG_MIN;
    uint64_t min_realtime_clock = ULLONG_MAX;
    uint64_t max_realtime_clock = 0;
    uint64_t rt_frequency = 0;
};

static bool time_in_range(const Options& opt, int64_t t)
{
    return t >= opt.min_time && t <= opt.max_time;
}

static rocprofiler_thread_trace_decoder_status_t isa_callback(
    char* instruction,
    uint64_t* memory_size,
    uint64_t* size,
    rocprofiler_thread_trace_decoder_pc_t,
    void*)
{
    const char* text = "s_nop 0";
    const auto  need = static_cast<uint64_t>(std::strlen(text) + 1);
    if(*size < need)
    {
        *size = need;
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    std::memcpy(instruction, text, need);
    *size        = need - 1;
    *memory_size = 4;
    return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
}

static rocprofiler_thread_trace_decoder_status_t trace_callback(
    rocprofiler_thread_trace_decoder_record_type_t record_type,
    void* trace_events,
    uint64_t trace_size,
    void* userdata)
{
    auto* state = reinterpret_cast<State*>(userdata);
    if(static_cast<int>(record_type) >= 0 && static_cast<int>(record_type) < 64)
        state->record_counts[static_cast<int>(record_type)] += trace_size;

    if(trace_events == nullptr)
    {
        if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_RT_FREQUENCY && trace_size >= 1)
            state->rt_frequency = reinterpret_cast<uint64_t>(trace_events);
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_RT_FREQUENCY)
    {
        if(trace_size >= 1) state->rt_frequency = reinterpret_cast<uint64_t*>(trace_events)[0];
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_REALTIME)
    {
        auto* rows = reinterpret_cast<rocprofiler_thread_trace_decoder_realtime_t*>(trace_events);
        for(uint64_t i = 0; i < trace_size; ++i)
        {
            state->realtime_out.push_back(RealtimeRow{rows[i].shader_clock, rows[i].realtime_clock, rows[i].reserved});
            state->min_realtime_shader_clock = std::min<int64_t>(state->min_realtime_shader_clock, rows[i].shader_clock);
            state->max_realtime_shader_clock = std::max<int64_t>(state->max_realtime_shader_clock, rows[i].shader_clock);
            state->min_realtime_clock = std::min<uint64_t>(state->min_realtime_clock, rows[i].realtime_clock);
            state->max_realtime_clock = std::max<uint64_t>(state->max_realtime_clock, rows[i].realtime_clock);
        }
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_EVENT)
    {
        auto* rows = reinterpret_cast<rocprofiler_thread_trace_decoder_event_t*>(trace_events);
        for(uint64_t i = 0; i < trace_size; ++i)
            state->event_out.push_back(EventRow{rows[i].time, static_cast<uint32_t>(rows[i].type), rows[i].me_id, rows[i].pipe_id,
                                                rows[i].flags, rows[i].payload, rows[i].byte_offset});
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_OCCUPANCY)
    {
        auto* rows = reinterpret_cast<rocprofiler_thread_trace_decoder_occupancy_t*>(trace_events);
        for(uint64_t i = 0; i < trace_size; ++i)
            state->occupancy_out.push_back(OccupancyRow{rows[i].pc.address, rows[i].pc.code_object_id, rows[i].time,
                                                        rows[i].cu, rows[i].simd, rows[i].wave_id, rows[i].start,
                                                        rows[i].me_id, rows[i].pipe_id, rows[i].is_ext, rows[i].workgroup_id});
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_SHADERDATA)
    {
        auto* rows = reinterpret_cast<rocprofiler_thread_trace_decoder_shaderdata_t*>(trace_events);
        for(uint64_t i = 0; i < trace_size; ++i)
            state->shaderdata_out.push_back(ShaderDataRow{rows[i].time, rows[i].value, rows[i].cu, rows[i].simd, rows[i].wave_id, rows[i].flags, rows[i].reserved});
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_PERFEVENT)
    {
        auto* rows = reinterpret_cast<rocprofiler_thread_trace_decoder_perfevent_t*>(trace_events);
        for(uint64_t i = 0; i < trace_size; ++i)
            state->perf_out.push_back(PerfRow{rows[i].time, rows[i].events0, rows[i].events1, rows[i].events2, rows[i].events3, rows[i].CU, rows[i].bank});
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type == ROCPROFILER_THREAD_TRACE_DECODER_RECORD_INST_OTHER_SIMD)
    {
        auto* rows = reinterpret_cast<rocprofiler_thread_trace_decoder_inst_other_simd_t*>(trace_events);
        for(uint64_t i = 0; i < trace_size; ++i)
            state->other_simd_out.push_back(OtherSimdRow{rows[i].size, rows[i].time, rows[i].cycles, rows[i].wgp, rows[i].category});
        return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
    }

    if(record_type != ROCPROFILER_THREAD_TRACE_DECODER_RECORD_WAVE) return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;

    auto* waves = reinterpret_cast<rocprofiler_thread_trace_decoder_wave_t*>(trace_events);
    for(uint64_t wi = 0; wi < trace_size; ++wi)
    {
        const auto& wave = waves[wi];
        const uint64_t wave_index = state->waves++;
        if(wave.begin_time < state->min_wave_begin) state->min_wave_begin = wave.begin_time;
        if(wave.end_time > state->max_wave_end) state->max_wave_end = wave.end_time;
        if(!state->options->wave_csv.empty())
        {
            state->waves_out.push_back(WaveRow{wave_index, wave.cu, wave.simd, wave.wave_id, wave.dispatcher, wave.workgroup_id,
                                               wave.begin_time, wave.end_time, wave.instructions_size});
        }

        int64_t prev_time = wave.begin_time;
        for(uint64_t ii = 0; ii < wave.instructions_size; ++ii)
        {
            const auto& inst = wave.instructions_array[ii];
            if(inst.pc.code_object_id == 0 && inst.pc.address == 0) continue;
            state->instructions_seen += 1;
            const int64_t inst_end = static_cast<int64_t>(inst.time) + static_cast<int64_t>(inst.duration);
            if(inst.time < state->min_inst_time) state->min_inst_time = inst.time;
            if(inst_end > state->max_inst_end) state->max_inst_end = inst_end;
            if(!time_in_range(*state->options, inst.time))
            {
                const int64_t end_time = inst_end;
                if(end_time > prev_time) prev_time = end_time;
                continue;
            }
            Key key{inst.pc.code_object_id, inst.pc.address};
            auto& out = state->by_pc[key];
            out.hitcount += 1;
            out.latency += inst.duration;
            out.stall += inst.stall;
            out.active += static_cast<int64_t>(inst.duration) - static_cast<int64_t>(inst.stall);
            const int64_t idle = inst.time - prev_time;
            if(idle > 0) out.idle += idle;
            const int64_t end_time = inst_end;
            if(end_time > prev_time) prev_time = end_time;
            out.category = inst.category;
            state->instructions_kept += 1;

            if(state->options->keep_raw)
            {
                state->raw_out.push_back(RawInstRow{wave_index, ii, wave.cu, wave.simd, wave.wave_id, inst.category,
                                                    inst.time, inst.duration, inst.stall,
                                                    static_cast<int64_t>(inst.duration) - static_cast<int64_t>(inst.stall),
                                                    idle > 0 ? idle : 0, inst.pc.code_object_id, inst.pc.address});
            }
        }
    }
    return ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS;
}

static std::vector<char> read_file(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if(!stream) throw std::runtime_error("failed to open input: " + path);
    return std::vector<char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

static void write_csv_aggregate(const std::string& path, const std::vector<std::pair<Key, Agg>>& rows)
{
    std::ofstream out(path);
    out << "code_object_id,pc_address,hitcount,latency,stall,active,idle,category\n";
    for(const auto& [key, agg] : rows)
        out << key.code_object_id << ',' << key.pc_address << ',' << agg.hitcount << ',' << agg.latency << ',' << agg.stall
            << ',' << agg.active << ',' << agg.idle << ',' << agg.category << '\n';
}

static void write_wave_csv(const std::string& path, const std::vector<WaveRow>& rows)
{
    std::ofstream out(path);
    out << "wave_index,cu,simd,wave_id,dispatcher,workgroup_id,begin_time,end_time,instructions\n";
    for(const auto& r : rows)
        out << r.wave_index << ',' << r.cu << ',' << r.simd << ',' << r.wave_id << ',' << r.dispatcher << ',' << r.workgroup_id << ','
            << r.begin_time << ',' << r.end_time << ',' << r.instructions << '\n';
}

static void write_raw_csv(const std::string& path, const std::vector<RawInstRow>& rows)
{
    std::ofstream out(path);
    out << "wave_index,instruction_index,cu,simd,wave_id,time,duration,stall,active,idle,category,code_object_id,pc_address\n";
    for(const auto& r : rows)
        out << r.wave_index << ',' << r.instruction_index << ',' << r.cu << ',' << r.simd << ',' << r.wave_id << ',' << r.time << ','
            << r.duration << ',' << r.stall << ',' << r.active << ',' << r.idle << ',' << r.category << ',' << r.code_object_id << ','
            << r.pc_address << '\n';
}

static void write_realtime_csv(const std::string& path, const std::vector<RealtimeRow>& rows)
{
    std::ofstream out(path); out << "shader_clock,realtime_clock,reserved\n";
    for(const auto& r : rows) out << r.shader_clock << ',' << r.realtime_clock << ',' << r.reserved << '\n';
}
static void write_event_csv(const std::string& path, const std::vector<EventRow>& rows)
{
    std::ofstream out(path); out << "time,type,me_id,pipe_id,flags,payload,byte_offset\n";
    for(const auto& r : rows) out << r.time << ',' << r.type << ',' << r.me_id << ',' << r.pipe_id << ',' << r.flags << ',' << r.payload << ',' << r.byte_offset << '\n';
}
static void write_occupancy_csv(const std::string& path, const std::vector<OccupancyRow>& rows)
{
    std::ofstream out(path); out << "address,code_object_id,time,cu,simd,wave_id,start,me_id,pipe_id,is_ext,workgroup_id\n";
    for(const auto& r : rows) out << r.address << ',' << r.code_object_id << ',' << r.time << ',' << r.cu << ',' << r.simd << ',' << r.wave_id << ',' << r.start << ',' << r.me_id << ',' << r.pipe_id << ',' << r.is_ext << ',' << r.workgroup_id << '\n';
}
static void write_shaderdata_csv(const std::string& path, const std::vector<ShaderDataRow>& rows)
{
    std::ofstream out(path); out << "time,value,cu,simd,wave_id,flags,reserved\n";
    for(const auto& r : rows) out << r.time << ',' << r.value << ',' << r.cu << ',' << r.simd << ',' << r.wave_id << ',' << r.flags << ',' << r.reserved << '\n';
}
static void write_perf_csv(const std::string& path, const std::vector<PerfRow>& rows)
{
    std::ofstream out(path); out << "time,events0,events1,events2,events3,cu,bank\n";
    for(const auto& r : rows) out << r.time << ',' << r.events0 << ',' << r.events1 << ',' << r.events2 << ',' << r.events3 << ',' << r.cu << ',' << r.bank << '\n';
}
static void write_other_simd_csv(const std::string& path, const std::vector<OtherSimdRow>& rows)
{
    std::ofstream out(path); out << "size,time,cycles,wgp,category\n";
    for(const auto& r : rows) out << r.size << ',' << r.time << ',' << r.cycles << ',' << r.wgp << ',' << r.category << '\n';
}

static std::string json_escape(const std::string& s)
{
    std::ostringstream o;
    for(char c : s)
    {
        if(c == '"') o << "\\\"";
        else if(c == '\\') o << "\\\\";
        else if(c == '\n') o << "\\n";
        else o << c;
    }
    return o.str();
}

static void write_summary_json(const std::string& path, rocprofiler_thread_trace_decoder_status_t status, const char* status_text, const State& state, size_t bytes)
{
    std::ofstream out(path);
    out << "{\n";
    out << "  \"status\": " << static_cast<int>(status) << ",\n";
    out << "  \"status_string\": \"" << json_escape(status_text ? status_text : "") << "\",\n";
    out << "  \"input_bytes\": " << bytes << ",\n";
    out << "  \"waves\": " << state.waves << ",\n";
    out << "  \"instructions_seen\": " << state.instructions_seen << ",\n";
    out << "  \"instructions_kept\": " << state.instructions_kept << ",\n";
    out << "  \"unique_pcs\": " << state.by_pc.size() << ",\n";
    out << "  \"min_wave_begin\": " << (state.min_wave_begin == LLONG_MAX ? 0 : state.min_wave_begin) << ",\n";
    out << "  \"max_wave_end\": " << (state.max_wave_end == LLONG_MIN ? 0 : state.max_wave_end) << ",\n";
    out << "  \"min_inst_time\": " << (state.min_inst_time == LLONG_MAX ? 0 : state.min_inst_time) << ",\n";
    out << "  \"max_inst_end\": " << (state.max_inst_end == LLONG_MIN ? 0 : state.max_inst_end) << ",\n";
    out << "  \"realtime_records\": " << state.realtime_out.size() << ",\n";
    out << "  \"min_realtime_shader_clock\": " << (state.min_realtime_shader_clock == LLONG_MAX ? 0 : state.min_realtime_shader_clock) << ",\n";
    out << "  \"max_realtime_shader_clock\": " << (state.max_realtime_shader_clock == LLONG_MIN ? 0 : state.max_realtime_shader_clock) << ",\n";
    out << "  \"min_realtime_clock\": " << (state.min_realtime_clock == ULLONG_MAX ? 0 : state.min_realtime_clock) << ",\n";
    out << "  \"max_realtime_clock\": " << state.max_realtime_clock << ",\n";
    out << "  \"rt_frequency\": " << state.rt_frequency << ",\n";
    out << "  \"event_records\": " << state.event_out.size() << ",\n";
    out << "  \"occupancy_records\": " << state.occupancy_out.size() << ",\n";
    out << "  \"shaderdata_records\": " << state.shaderdata_out.size() << ",\n";
    out << "  \"perf_records\": " << state.perf_out.size() << ",\n";
    out << "  \"other_simd_records\": " << state.other_simd_out.size() << ",\n";
    out << "  \"record_counts\": {";
    bool first = true;
    for(int i = 0; i < 64; ++i)
    {
        if(state.record_counts[i] == 0) continue;
        if(!first) out << ',';
        first = false;
        out << "\n    \"" << i << "\": " << state.record_counts[i];
    }
    if(!first) out << '\n' << "  ";
    out << "}\n";
    out << "}\n";
}

static bool parse_int64(const std::string& s, int64_t& out)
{
    try { size_t pos = 0; out = std::stoll(s, &pos, 0); return pos == s.size(); }
    catch(...) { return false; }
}

static bool parse_args(int argc, char** argv, Options& opt)
{
    if(argc < 3) return false;
    opt.input_path = argv[1]; opt.out_csv = argv[2];
    for(int i = 3; i < argc; ++i)
    {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* { if(i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name); return argv[++i]; };
        if(a == "--summary-json") opt.summary_json = need("--summary-json");
        else if(a == "--wave-csv") opt.wave_csv = need("--wave-csv");
        else if(a == "--raw-instruction-csv") { opt.raw_instruction_csv = need("--raw-instruction-csv"); opt.keep_raw = true; }
        else if(a == "--realtime-csv") opt.realtime_csv = need("--realtime-csv");
        else if(a == "--event-csv") opt.event_csv = need("--event-csv");
        else if(a == "--occupancy-csv") opt.occupancy_csv = need("--occupancy-csv");
        else if(a == "--shaderdata-csv") opt.shaderdata_csv = need("--shaderdata-csv");
        else if(a == "--perf-csv") opt.perf_csv = need("--perf-csv");
        else if(a == "--other-simd-csv") opt.other_simd_csv = need("--other-simd-csv");
        else if(a == "--all-record-csv")
        {
            std::string stem = need("--all-record-csv");
            opt.wave_csv = stem + ".waves.csv";
            opt.raw_instruction_csv = stem + ".instructions.csv"; opt.keep_raw = true;
            opt.realtime_csv = stem + ".realtime.csv";
            opt.event_csv = stem + ".events.csv";
            opt.occupancy_csv = stem + ".occupancy.csv";
            opt.shaderdata_csv = stem + ".shaderdata.csv";
            opt.perf_csv = stem + ".perf.csv";
            opt.other_simd_csv = stem + ".other_simd.csv";
        }
        else if(a == "--min-time") { int64_t v; if(!parse_int64(need("--min-time"), v)) throw std::runtime_error("bad --min-time"); opt.min_time = v; }
        else if(a == "--max-time") { int64_t v; if(!parse_int64(need("--max-time"), v)) throw std::runtime_error("bad --max-time"); opt.max_time = v; }
        else if(a == "--help") return false;
        else throw std::runtime_error("unknown argument: " + a);
    }
    return true;
}

int main(int argc, char** argv)
{
    Options opt;
    try
    {
        if(!parse_args(argc, argv, opt))
        {
            std::cerr << "usage: rocprof_trace_aggregate <input.att-or-sqtt.bin> <out.csv> [--summary-json path] [--wave-csv path] [--raw-instruction-csv path] [--realtime-csv path] [--event-csv path] [--occupancy-csv path] [--shaderdata-csv path] [--perf-csv path] [--other-simd-csv path] [--all-record-csv stem] [--min-time t] [--max-time t]\n";
            return 2;
        }
    }
    catch(const std::exception& e) { std::cerr << e.what() << "\n"; return 2; }

    std::vector<char> data;
    try { data = read_file(opt.input_path); }
    catch(const std::exception& e) { std::cerr << e.what() << "\n"; return 2; }

    rocprof_trace_decoder_handle_t handle{};
    auto status = rocprof_trace_decoder_create_handle(&handle);
    if(status != ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS) { std::cerr << "create_handle failed: " << status << "\n"; return 1; }
    status = rocprof_trace_decoder_set_isa_callback(handle, isa_callback, nullptr);
    if(status != ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS) { std::cerr << "set_isa_callback failed: " << status << "\n"; return 1; }

    State state{}; state.options = &opt;
    status = rocprof_trace_decoder_parse(handle, data.data(), data.size(), trace_callback, &state);
    const char* status_text = rocprof_trace_decoder_get_status_string(status);
    std::cerr << "status=" << status << " " << (status_text ? status_text : "") << " waves=" << state.waves
              << " instructions_seen=" << state.instructions_seen << " instructions_kept=" << state.instructions_kept
              << " unique_pcs=" << state.by_pc.size() << " min_inst_time=" << (state.min_inst_time == LLONG_MAX ? 0 : state.min_inst_time)
              << " max_inst_end=" << (state.max_inst_end == LLONG_MIN ? 0 : state.max_inst_end)
              << " realtime_records=" << state.realtime_out.size() << " event_records=" << state.event_out.size() << "\n";
    for(int i = 0; i < 64; ++i) if(state.record_counts[i] != 0) std::cerr << "record_" << i << "=" << state.record_counts[i] << "\n";

    std::vector<std::pair<Key, Agg>> rows(state.by_pc.begin(), state.by_pc.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.latency > b.second.latency; });
    write_csv_aggregate(opt.out_csv, rows);
    if(!opt.wave_csv.empty()) write_wave_csv(opt.wave_csv, state.waves_out);
    if(!opt.raw_instruction_csv.empty()) write_raw_csv(opt.raw_instruction_csv, state.raw_out);
    if(!opt.realtime_csv.empty()) write_realtime_csv(opt.realtime_csv, state.realtime_out);
    if(!opt.event_csv.empty()) write_event_csv(opt.event_csv, state.event_out);
    if(!opt.occupancy_csv.empty()) write_occupancy_csv(opt.occupancy_csv, state.occupancy_out);
    if(!opt.shaderdata_csv.empty()) write_shaderdata_csv(opt.shaderdata_csv, state.shaderdata_out);
    if(!opt.perf_csv.empty()) write_perf_csv(opt.perf_csv, state.perf_out);
    if(!opt.other_simd_csv.empty()) write_other_simd_csv(opt.other_simd_csv, state.other_simd_out);
    if(!opt.summary_json.empty()) write_summary_json(opt.summary_json, status, status_text, state, data.size());

    rocprof_trace_decoder_destroy_handle(handle);
    return status == ROCPROFILER_THREAD_TRACE_DECODER_STATUS_SUCCESS ? 0 : 1;
}
