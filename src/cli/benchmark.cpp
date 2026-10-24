#include "x64arm/benchmark.hpp"
#include "x64arm/build_info.hpp"
#include "x64arm/aot_engine.hpp"
#include "x64arm/platform.hpp"
#include "backend_policy.hpp"
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
struct Measurements { std::vector<double> interpreter, cold, warm; AotEngine::Stats stats; };
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
    cli::reject_retired_backends();
    AotEngine jit;
    RunOptions native_options;native_options.max_instructions=UINT64_MAX;
    output << std::fixed << std::setprecision(3)
        << "{\n  \"schema\":2,\"backend\":\"resident-aot\",\"suite\":\"core\",\"samples\":" << samples
        << ",\"clock\":\"steady_clock\",\"configuration\":" << json_string(build::configuration)
        << ",\"compiler\":" << json_string(build::compiler) << ",\"compiler_flags\":" << json_string(build::flags)
        << ",\"host_report\":" << json_string(host)
        << ",\n  \"timing_scope\":\"單次 run；排除讀檔、客體配置、快照還原、AOT arena 配置、結果比對。冷執行含解碼、轉譯、callback 發布、執行；暖執行重用同一 address space 的已暖機快取。\",\n  \"workloads\":[\n";
    bool first = true;
    for (const auto name : {"sum100", "branches", "memory_copy"}) {
        auto program = prepare_raw(read_raw_file(fixtures / (std::string(name) + ".bin")));
        const auto initial = program.state; const auto snapshot = program.memory;
        auto expected_state = initial; auto expected_memory = snapshot;
        const auto expected = Interpreter{}.run(expected_state, expected_memory);
        if (expected.reason != StopReason::Returned) throw std::runtime_error("基準 fixture 未正常返回");
        const auto reset = [&] { program.state = initial; program.memory.restore_writable(snapshot); };
        const auto verify = [&](const ExecutionResult& result) {
            auto reference_state=expected_state;
            if(!result.completed_exact){
                // 這三個 fixture 沒有 RDTSC；resident 不逐條累計虛擬 TSC。
                // 記憶體／指令回退可更新 TSC，正常原生本體不保證逐條計數。
                reference_state.tsc=program.state.tsc;
            }
            if (result.reason != expected.reason || result.rip != expected.rip ||
                program.state != reference_state || !program.memory.same_contents(expected_memory))
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
            const auto result = jit.run(program.state, program.memory,native_options);
            const auto end = Clock::now(); verify(result);
            measured.cold.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        reset(); jit.clear(); verify(jit.run(program.state, program.memory,native_options));
        const auto before = jit.stats();
        for (unsigned n = 0; n < samples; ++n) {
            reset(); const auto start = Clock::now();
            const auto result = jit.run(program.state, program.memory,native_options);
            const auto end = Clock::now(); verify(result);
            measured.warm.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        measured.stats = jit.stats();
        if(!measured.stats.resident_runs)throw std::runtime_error("benchmark 未使用 resident AOT");
        if (measured.stats.modules != before.modules || measured.stats.precise_modules != before.precise_modules) throw std::runtime_error("暖執行意外重編譯模組");
        output << (first ? "" : ",\n") << "    {\"name\":" << json_string(name) << ",\"reference_instructions\":" << expected.completed
            << ",\"RAX\":" << expected_state.reg(Gpr::Rax) << ",\"correctness\":\"match\",\"interpreter\":";
        distribution(output, measured.interpreter); output << ",\"cold_aot\":"; distribution(output, measured.cold);
        output << ",\"warm_aot\":"; distribution(output, measured.warm);
        output << ",\"warm_native_entries\":" << measured.stats.native_entries - before.native_entries
            << ",\"warm_resident_runs\":" << measured.stats.resident_runs - before.resident_runs
            << ",\"warm_fallback_instructions\":" << measured.stats.fallback_instructions - before.fallback_instructions << '}';
        first = false;
    }
    output << "\n  ]\n}\n";
}
}
