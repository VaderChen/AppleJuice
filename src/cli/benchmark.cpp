#include "x64arm/benchmark.hpp"
#include "x64arm/build_info.hpp"
#include "x64arm/jit.hpp"
#include "x64arm/raw_program.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace x64arm {
namespace {
using Clock = std::chrono::steady_clock;
std::string json_string(std::string_view text) {
    std::ostringstream out; out << '"';
    for (char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
        else out << c;
    }
    out << '"'; return out.str();
}
struct Measurements { std::vector<double> interpreter, cold, warm; JitStats stats; };
void distribution(std::ostream& out, const std::vector<double>& samples) {
    auto sorted = samples; std::sort(sorted.begin(), sorted.end());
    const auto p95 = (sorted.size() * 95 + 99) / 100 - 1;
    const auto median = sorted.size() % 2 ? sorted[sorted.size() / 2] : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2;
    out << "{\"min_us\":" << sorted.front() << ",\"median_us\":" << median
        << ",\"p95_us\":" << sorted[p95] << ",\"max_us\":" << sorted.back() << ",\"samples_us\":[";
    for (std::size_t i = 0; i < samples.size(); ++i) out << (i ? "," : "") << samples[i];
    out << "]}";
}
}
void benchmark_core(const std::filesystem::path& fixtures, unsigned samples, std::ostream& output) {
    if (samples < 3 || samples > 1000) throw std::invalid_argument("benchmark samples 必須介於 3 與 1000");
    const auto host = doctor_report();
    JitEngine jit;
    output << std::fixed << std::setprecision(3)
        << "{\n  \"schema\":1,\"suite\":\"core\",\"samples\":" << samples
        << ",\"clock\":\"steady_clock\",\"configuration\":" << json_string(build::configuration)
        << ",\"compiler\":" << json_string(build::compiler) << ",\"compiler_flags\":" << json_string(build::flags)
        << ",\"host_report\":" << json_string(host)
        << ",\n  \"timing_scope\":\"單次 run；排除讀檔、客體配置、快照還原、JIT arena 配置、結果比對。冷執行含解碼、轉譯、callback 發布、執行；暖執行重用同一 address space 的已暖機快取。\",\n  \"workloads\":[\n";
    bool first = true;
    for (const auto name : {"sum100", "branches", "memory_copy"}) {
        auto program = prepare_raw(read_raw_file(fixtures / (std::string(name) + ".bin")));
        const auto initial = program.state; const auto snapshot = program.memory;
        auto expected_state = initial; auto expected_memory = snapshot;
        const auto expected = Interpreter{}.run(expected_state, expected_memory);
        if (expected.reason != StopReason::Returned) throw std::runtime_error("基準 fixture 未正常返回");
        const auto reset = [&] { program.state = initial; program.memory.restore_writable(snapshot); };
        const auto verify = [&](const ExecutionResult& result) {
            if (result.reason != expected.reason || result.completed != expected.completed ||
                program.state != expected_state || !program.memory.same_contents(expected_memory))
                throw std::runtime_error("benchmark 結果與直譯器參考不一致");
        };
        Measurements measured;
        for (unsigned n = 0; n < samples; ++n) {
            reset(); const auto start = Clock::now();
            const auto result = Interpreter{}.run(program.state, program.memory);
            const auto end = Clock::now(); verify(result);
            measured.interpreter.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        for (unsigned n = 0; n < samples; ++n) {
            reset(); jit.clear(); const auto start = Clock::now();
            const auto result = jit.run(program.state, program.memory);
            const auto end = Clock::now(); verify(result);
            measured.cold.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        reset(); jit.clear(); verify(jit.run(program.state, program.memory));
        const auto before = jit.stats();
        for (unsigned n = 0; n < samples; ++n) {
            reset(); const auto start = Clock::now();
            const auto result = jit.run(program.state, program.memory);
            const auto end = Clock::now(); verify(result);
            measured.warm.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        measured.stats = jit.stats();
        if (measured.stats.translated_blocks != before.translated_blocks) throw std::runtime_error("暖執行意外重編譯區塊");
        output << (first ? "" : ",\n") << "    {\"name\":" << json_string(name) << ",\"completed\":" << expected.completed
            << ",\"RAX\":" << expected_state.reg(Gpr::Rax) << ",\"correctness\":\"match\",\"interpreter\":";
        distribution(output, measured.interpreter); output << ",\"cold_jit\":"; distribution(output, measured.cold);
        output << ",\"warm_jit\":"; distribution(output, measured.warm);
        output << ",\"warm_cache_hits\":" << measured.stats.cache_hits - before.cache_hits
            << ",\"warm_native_instructions\":" << measured.stats.native_instructions - before.native_instructions
            << ",\"warm_fallback_instructions\":" << measured.stats.fallback_instructions - before.fallback_instructions << '}';
        first = false;
    }
    output << "\n  ]\n}\n";
}
}
