#include "x64arm/startup_profile.hpp"
#include "engine_bundle.hpp"
#include "apflg_probe.hpp"
#include "../platform/host_file_limit.hpp"
#include "x64arm/native_application.hpp"
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "x64arm/gpu_memory.hpp"
#include "x64arm/resource_usage.hpp"
#if defined(__APPLE__)
#include "x64arm/resource_cache.hpp"
#endif
#include "x64arm/api_audit.hpp"
#include <csignal>
#include "backend_policy.hpp"
#include "x64arm/platform.hpp"
#include "x64arm/aot_engine.hpp"
#include "x64arm/cross_architecture.hpp"
#include "x64arm/execution_policy.hpp"
#include "x64arm/native_memory_scope.hpp"
#include "x64arm/benchmark.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/pe_loader.hpp"
#include "x64arm/raw_program.hpp"
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <algorithm>
#include <string_view>

namespace {
using namespace x64arm;
volatile std::sig_atomic_t analysis_cancelled=0;
void usage() {
    std::cout << "x64arm：自製 x64 → ARM64 執行引擎\n"
        "用法：\n  x64arm doctor\n  x64arm jit-smoke\n  x64arm apflg-probe（實測宿主 AF／PF 旗標）\n"
        "  x64arm run-raw <檔案.bin> --load-address <地址> --entry <地址> [--backend interpreter|jit]\n"
        "  x64arm run-pe <檔案.exe> [--backend interpreter|jit] [--image-base <地址>]\n"
        "  x64arm audit-pe <EXE或DLL>（唯讀 JSON 匯入／匯出與 API 登錄盤點）\n"
        "  x64arm inspect-pe <檔案.exe>（載入檢查與未解析匯入清單，不執行客體）\n"
        "  x64arm compare <檔案.bin|檔案.exe> [對應的載入參數]\n"
        "  x64arm bench --suite core [--samples <3..1000>] [--fixtures <目錄>]\n"
        "PE 選項：--command-line <完整 Windows 命令列>、--fs-root <共享目錄>\n"
        "檔案模式：--fs-mode read-only|read-write；read-write 允許修改映射目錄。\n"
        "附加磁碟：--drive D=/目錄，可重複指定 D-Z；子程序繼承映射。\n"
        "登錄保存：--registry-file <檔案>，與子程序共用虛擬登錄資料\n"
        "C 槽映射：--c-drive <目錄>，支援 C:\\ 與根目錄路徑，預設唯讀。\n"
        "根憑證檔案：--certificate-roots-file <C 槽相對路徑>，預設關閉並使用內建公開 CA。\n"
        "安裝目錄：--install-root <目錄>，映射 C:\\Program Files，保留程式自己的子目錄。\n"
        "即時管線：--stdio live；預設 capture 將輸出保留到結束。\n"
        "實體多核：--cpu-count auto 或核心數；需搭配 JIT 與 --clock host。\n"
        "原生函式庫實驗：X64ARM_NATIVE_LIBRARIES=zlib 啟用 zlib1.dll 無狀態函式代理。\n"
        "執行診斷：--diagnostics-file <檔案>；run-pe 將引擎診斷另存，保留客體 stdout／stderr。\n"
        "資源用量：--usage-file <檔案>；run-pe 每 3 秒更新此程序的 CPU／GPU／MEM／NET JSON 快照。\n"
        "標準輸入：--stdin-file <檔案|->；- 表示讀取宿主 stdin 快照。\n"
        "相容性診斷：--unresolved-imports reject|trap；trap 在實際呼叫未知 API 時停止，預設 reject。\n"
        "預編譯：precompile-pe <EXE或DLL> [--scan-dlls adjacent] [--profile <熱點報告>] [--ordering host] [--image-base <位址>]，不執行客體程式\n"
        "32／64 位元預設使用 resident AOT，預設不讀寫持久快取；X64ARM_AOT_PERSISTENT=1 才啟用 .smap，手動預編譯產生 .aot.smap。\n"
        "舊 JIT／LLVM 執行入口已封閉，只保留原始碼參考；--backend jit 固定使用 resident AOT。\n"
        "X64ARM_JIT_MODULE_AOT 僅相容值 1；舊 JIT／LLVM 啟用設定會明確拒絕。\n"
        "背景編譯：X64ARM_JIT_COMPILER_WORKERS=0..8（0 同步；即時與離線編譯預設 2）。\n"
        "共用選項：--max-instructions <步數>（正常 resident AOT 預設無上限，其他模式預設 10000000）、--dump-jit <目錄>\n"
        "GPU 記憶體：--gpu-memory-mib <MiB>（至少 16，至多宿主實體記憶體的 50%；預設 256，按需配置）\n"
        "PE 記憶體：--memory-limit-mib <64..8192>（預設 1024，只按實際配置提交）\n"
        "raw 選項：--stack-address <地址>、--stack-size <bytes>\n"
        "PE GUI：--gui headless|native；native 可搭配 --gui-snapshot <PNG路徑>。compare 僅使用 headless。\n"
        "PE 時鐘：--clock host|virtual；run-pe 預設 host，以宿主硬體時鐘計時；virtual 依指令數推進，供可重現測試使用。\n"
        "數值接受十進位或 0x 十六進位。PE 支援受控 AMD64 console／GUI 與已列明的 CRT／Win32 子集。\n";
}
std::uint64_t number(std::string_view text) {
    const auto original = text;
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) { base = 16; text.remove_prefix(2); }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("數值不合法：" + std::string(original));
    return value;
}
unsigned compiler_worker_count(){
    // 即時與離線編譯預設兩個 worker；呼叫端可明確覆寫。
    unsigned count=2;
    if(const auto* configured=std::getenv("X64ARM_JIT_COMPILER_WORKERS");configured && *configured){
        const auto workers=number(configured);if(workers>8)throw std::invalid_argument("X64ARM_JIT_COMPILER_WORKERS 必須介於 0–8");
        count=static_cast<unsigned>(workers);
    }
    return count;
}
bool equal_result(const ExecutionResult& a, const ExecutionResult& b) {
    return a.reason == b.reason && a.completed == b.completed && a.rip == b.rip && a.exit_code == b.exit_code &&
        a.fault_address == b.fault_address && a.fault_access == b.fault_access && a.fault_size == b.fault_size;
}
ExecutionResult execute_module(GuestState& state,GuestMemory& memory,RunOptions options,
                               const std::filesystem::path& dump,std::ostream& diagnostics,const std::filesystem::path& source){
    const auto workers=compiler_worker_count();
    if(!std::getenv("X64ARM_JIT_COMPILER_WORKERS"))::setenv("X64ARM_JIT_COMPILER_WORKERS",std::to_string(workers).c_str(),1);
    diagnostics<<"aot_compiler_workers="<<(std::getenv("X64ARM_AOT_BACKGROUND_COMPILE")?std::getenv("X64ARM_AOT_BACKGROUND_COMPILE"):std::getenv("X64ARM_JIT_COMPILER_WORKERS"))<<'\n';
    AotEngine engine;ExecutionResult result;std::uint64_t completed=0;
    const auto* setting=std::getenv("X64ARM_AOT_PERSISTENT");
    const bool persistent=!source.empty() && setting && std::string_view(setting)=="1";
    if(persistent)diagnostics<<"aot_smap_loaded="<<engine.open_smap(std::filesystem::absolute(source))<<'\n';
    const auto* configured=std::getenv("X64ARM_PROGRESS_INSTRUCTIONS");
    const auto interval=configured && *configured?number(configured):options.max_instructions;
    if(configured && *configured && !interval)throw std::invalid_argument("進度診斷間隔不得為零");
    const auto started=std::chrono::steady_clock::now();startup_profile::mark("aot_module_execute_begin");
    do{
        auto slice=options;slice.max_instructions=std::min(interval,options.max_instructions-completed);
        slice.diagnose_budget=slice.max_instructions==options.max_instructions-completed && options.diagnose_budget;
        result=engine.run(state,memory,slice);completed+=result.completed;
        if(configured && *configured)diagnostics<<"execution_progress host_unix_us="<<startup_profile::unix_us()<<" instructions="<<completed
            <<" elapsed_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<' '<<format_stats(engine.stats())<<std::flush;
        if(result.reason!=StopReason::BudgetExhausted || !result.completed)break;
    }while(completed<options.max_instructions);
    if(persistent)diagnostics<<"aot_smap_saved="<<engine.save_smap()<<'\n';
    result.completed=completed;if(!dump.empty())engine.dump(dump);diagnostics<<format_stats(engine.stats());return result;
}
ExecutionResult execute(bool jit, GuestState& state, GuestMemory& memory, RunOptions options,
                        const std::filesystem::path& dump, std::ostream& diagnostics, const std::filesystem::path& source = {}) {
    if (!jit) return Interpreter{}.run(state, memory, options);
    cli::reject_retired_backends();
    return execute_module(state,memory,options,dump,diagnostics,source);
}
int compare(GuestState state, GuestMemory memory, RunOptions options, WindowsServices* services,
            const std::filesystem::path& dump) {
    auto reference_state = state;
    auto reference_memory = memory;
    WindowsServices reference_services;
    if (services) { services->restrict_comparison();reference_services = *services; options.services = &reference_services; }
    const auto reference = Interpreter{}.run(reference_state, reference_memory, options);
    options.services = services;
    const auto actual = execute(true, state, memory, options, dump, std::cout);
    const bool matched = equal_result(reference, actual) && reference_state == state && reference_memory.same_contents(memory) &&
        (!services || reference_services.same_state(*services));
    std::cout << "compare=" << (matched ? "match" : "mismatch") << '\n' << format_result(actual, state);
    if (!matched) std::cerr << "直譯器參考結果：\n" << format_result(reference, reference_state);
    return matched ? 0 : 1;
}
}
int main(int argc, char** argv) {
    using namespace x64arm;
    startup_profile::mark("main_enter");
    std::ofstream diagnostics_file;
    std::ostream* diagnostics=&std::cerr;
    try {
        cli::enter_authorized_bundle(argv);
        initialize_cross_architecture(argv);
        HostExecutionScope host_execution;
        if(const auto* qos=std::getenv("X64ARM_HOST_QOS");qos && std::string_view(qos)=="responsive"){
            if(!host_execution.apply(ExecutionPolicy::Responsive))throw std::runtime_error("無法設定宿主互動工作優先級");
            std::clog<<"host_qos=responsive\n";
        }
        if (argc < 2) { usage(); return 2; }
        const std::string_view command = argv[1];
        if(command=="run-pe" || command=="run-raw" || command=="compare" || command=="precompile-pe" || command=="bench")cli::reject_retired_backends();
        const auto host_files = command == "run-pe" ? prepare_host_file_limit() : HostFileLimit{};
#if defined(__APPLE__)
        if(command=="run-pe"){startup_profile::Scope measurement("startup","resource_preload",true);preload_generated_resources_async();}
#endif
        if (command == "--help" || command == "-h" || command == "help") { usage(); return argc == 2 ? 0 : 2; }
        if (command == "audit-pe") {
            if (argc != 3) throw std::invalid_argument("audit-pe 需要一個 EXE／DLL 路徑");
            audit_pe(argv[2], std::cout); return 0;
        }
        if (command == "doctor") {
            if (argc != 2) throw std::invalid_argument("doctor 不接受其他參數");
            std::cout << doctor_report(); return 0;
        }
        if (command == "apflg-probe") {
            if (argc != 2) throw std::invalid_argument("apflg-probe 不接受其他參數");
            return cli::apflg_probe();
        }
        if (command == "jit-smoke") {
            if (argc != 2) throw std::invalid_argument("jit-smoke 不接受其他參數");
            const auto probe = probe_jit();
            if (probe.status == ProbeStatus::Ok) { std::cout << "JIT result=" << probe.value << '\n' << probe.message << '\n'; return 0; }
            std::cerr << (probe.status == ProbeStatus::Unavailable ? "unsupported/skip：" : "JIT 失敗：") << probe.message << '\n';
            return probe.status == ProbeStatus::Unavailable ? 77 : 1;
        }
        if (command == "bench") {
            unsigned samples = 25;
            std::filesystem::path fixtures = X64ARM_FIXTURES;
            std::set<std::string_view> options;
            for (int i = 2; i < argc; i += 2) {
                const std::string_view option = argv[i];
                if (i + 1 >= argc || !options.insert(option).second) throw std::invalid_argument("bench 參數缺值或重複");
                const std::string_view value = argv[i + 1];
                if (option == "--suite") {
                    if (value != "core") throw std::invalid_argument("bench 目前只支援 core suite");
                } else if (option == "--samples") {
                    const auto count = number(value);
                    if (count < 3 || count > 1000) throw std::invalid_argument("benchmark samples 必須介於 3 與 1000");
                    samples = static_cast<unsigned>(count);
                } else if (option == "--fixtures") fixtures = value;
                else throw std::invalid_argument("未知 bench 參數");
            }
            benchmark_core(fixtures, samples, std::cout);
            return 0;
        }
        if(command=="precompile-pe"){
            std::signal(SIGTERM,[](int){analysis_cancelled=1;});
            std::signal(SIGINT,[](int){analysis_cancelled=1;});
            if(argc<3)throw std::invalid_argument("precompile-pe <EXE或DLL> [--scan-dlls adjacent] [--profile <熱點報告>] [--ordering host] [--image-base <位址>]");
            bool adjacent=false;std::filesystem::path profile_path;std::optional<GuestAddress> precompile_base;
            for(int n=3;n<argc;++n){const std::string_view option=argv[n];
                if(option=="--scan-dlls" && n+1<argc && std::string_view(argv[n+1])=="adjacent"){adjacent=true;++n;}
                else if(option=="--profile" && n+1<argc)profile_path=argv[++n];
                else if(option=="--ordering" && n+1<argc && std::string_view(argv[n+1])=="host"){++n;}
                else if(option=="--image-base" && n+1<argc)precompile_base=GuestAddress{number(argv[++n])};
                else throw std::invalid_argument("未知預編譯參數");
            }
            if(adjacent && precompile_base)throw std::invalid_argument("指定重定位位址時只能預編譯單一映像");
            struct HotEntry {std::uint64_t rip{},calls{},instructions{};std::string bytes;};std::vector<HotEntry> hot;
            if(!profile_path.empty()){
                if(std::filesystem::file_size(profile_path)>16U*1024U*1024U)throw std::invalid_argument("熱點報告超過 16 MiB");
                std::ifstream profile(profile_path);if(!profile)throw std::invalid_argument("無法讀取熱點報告");
                std::string line;while(std::getline(profile,line)){
                    std::istringstream row(line);std::string kind,field;row>>kind;if(kind!="native" && kind!="mixed")continue;
                    HotEntry entry;
                    while(row>>field){
                        if(field.starts_with("rip="))entry.rip=number(std::string_view(field).substr(4));
                        else if(field.starts_with("calls="))entry.calls=number(std::string_view(field).substr(6));
                        else if(field.starts_with("instructions="))entry.instructions=number(std::string_view(field).substr(13));
                        else if(field.starts_with("bytes=")){entry.bytes=field.substr(6);break;}
                    }
                    if(entry.calls>=64 && !entry.bytes.empty() && entry.bytes.size()<=30 && entry.bytes.size()%2==0)hot.push_back(std::move(entry));
                }
                if(hot.empty())throw std::invalid_argument("熱點報告沒有含指令 bytes 且 calls >= 64 的原生入口；請以新版引擎重新收集");
                std::sort(hot.begin(),hot.end(),[](const auto& a,const auto& b){return a.instructions>b.instructions;});
            }
            std::vector<std::filesystem::path> sources{std::filesystem::absolute(argv[2])};
            if(adjacent)for(const auto& item:std::filesystem::directory_iterator(sources.front().parent_path())){
                auto ext=item.path().extension().string();for(auto& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if(ext==".dll" && item.path()!=sources.front())sources.push_back(item.path());
            }
            bool passed=true;std::size_t finished=0;
            for(const auto& source:sources){
                if(analysis_cancelled)return 130;
                std::cout<<"smap_progress="<<finished<<'/'<<sources.size()<<" phase=scan source="<<source<<std::endl;
                try{
                const auto bytes=read_pe_snapshot(source);auto ext=source.extension().string();for(auto& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const auto image=parse_pe_image(*bytes,precompile_base,ext==".dll");
                GuestMemory memory;
                memory.set_execution_mode(image.metadata.architecture==PeArchitecture::X86?ExecutionMode::X86:ExecutionMode::X64);
                map_pe_image(memory,image);
                std::vector<GuestAddress> seeds{image.metadata.entry};
                for(const auto& fn:image.runtime.functions)seeds.push_back(GuestAddress{image.metadata.loaded_base.value+fn.begin});
                for(const auto& [ordinal,rva]:image.runtime.export_ordinals)if(!image.runtime.export_forwarders.contains(ordinal))seeds.push_back(GuestAddress{image.metadata.loaded_base.value+rva});
                for(const auto entry:image.runtime.tls_callbacks)seeds.push_back(GuestAddress{entry});
                if(!profile_path.empty()){
                    seeds.clear();std::set<std::uint64_t> seen;
                    std::vector<std::pair<GuestAddress,std::uint64_t>> matched;long double total=0;
                    for(const auto& entry:hot){
                        if(entry.rip<image.metadata.loaded_base.value || entry.rip-image.metadata.loaded_base.value>=image.metadata.image_size || !seen.insert(entry.rip).second)continue;
                        try{
                            std::vector<std::uint8_t> expected;for(std::size_t n=0;n<entry.bytes.size();n+=2)expected.push_back(static_cast<std::uint8_t>(number("0x"+entry.bytes.substr(n,2))));
                            std::vector<std::uint8_t> actual(expected.size());memory.read(GuestAddress{entry.rip},actual,Access::Execute);
                            if(actual==expected && entry.instructions){matched.emplace_back(GuestAddress{entry.rip},entry.instructions);total+=static_cast<long double>(entry.instructions);}
                        }catch(const MemoryFault&){}

                    }
                    long double covered=0;
                    for(const auto& [rip,weight]:matched){
                        seeds.push_back(rip);covered+=static_cast<long double>(weight);
                        if(covered>=total*0.95L || seeds.size()==1024)break;
                    }
                    if(seeds.empty()){std::cout<<"profile_skipped="<<source<<" reason=no_matching_hot_entries"<<std::endl;++finished;continue;}
                    std::cout<<"profile_selected="<<seeds.size()<<" candidates="<<matched.size()<<" instruction_coverage="<<static_cast<double>(covered/total)<<std::endl;
                }
                // 熱點模式重新產生精簡快取，不把舊的全檔掃描記錄帶回來。
                {
                    std::vector<TranslationRegion> regions;
                    for(const auto& function:image.runtime.functions){
                        if(function.begin==function.end)continue;
                        const TranslationRegion region{GuestAddress{image.metadata.loaded_base.value+function.begin},
                            GuestAddress{image.metadata.loaded_base.value+function.end}};
                        if(!regions.empty() && regions.back().begin==region.begin && regions.back().end==region.end)continue;
                        regions.push_back(region);
                    }
                    AotEngine engine;const bool loaded=engine.open_prepared_smap(source,profile_path.empty());
                    std::cout<<"precompile_backend=resident-aot precompile_ordering=host guest_executed=0\n";
                    std::cout<<"smap_progress="<<finished<<'/'<<sources.size()<<" phase=compile source="<<source<<std::endl;
                    const auto modules=engine.precompile(memory,seeds,image.metadata.loaded_base,image.metadata.image_size,
                        regions,[]{return analysis_cancelled!=0;},profile_path.empty());
                    if(analysis_cancelled)return 130;
                    std::cout<<"smap_progress="<<finished<<'/'<<sources.size()<<" phase=save source="<<source<<std::endl;
                    const bool saved=engine.save_smap();passed&=saved;
                    if(!saved)std::cerr<<"無法保存 resident AOT 快取。"<<std::endl;
                    std::cout<<"precompile_source="<<source<<" seeds="<<seeds.size()<<" smap_loaded="<<loaded
                        <<" prepared="<<modules<<" smap_saved="<<saved<<" preparation_limited="<<(engine.stats().prepared_rejections!=0)<<'\n'
                        <<format_stats(engine.stats());

                }
            }catch(const std::exception& error){passed=false;std::cerr<<source<<"："<<error.what()<<'\n';}
                ++finished;std::cout<<"smap_progress="<<finished<<'/'<<sources.size()<<" phase=complete"<<std::endl;}
            return passed?0:1;
        }
        const bool comparison = command == "compare";
        const bool inspection = command == "inspect-pe";
        if (command != "run-raw" && command != "run-pe" && !comparison && !inspection) throw std::invalid_argument("未知命令：" + std::string(command));
        if (argc < 3) throw std::invalid_argument("缺少程式檔案");
        RawConfig config;
        PeOptions pe_options;
        pe_options.trap_unresolved_imports = inspection;
        // 診斷後端保留有限預設；正常 resident AOT 在選定後端與時鐘後改為無配額。
        pe_options.memory_limit = std::size_t{1024} * 1024 * 1024;
        RunOptions run;
        run.max_instructions = 10'000'000;
        bool jit = command == "run-pe" || comparison;
        std::filesystem::path dump, gui_snapshot, stdin_file, usage_file, mapping_session_output;
        int process_environment_descriptor=-1;int process_token_descriptor=-1;int process_job_descriptor=-1;int process_session_descriptor=-1;std::string inherited_handles;std::string process_session;std::u16string process_directory;
        std::uint32_t startup_show=UINT32_MAX;std::size_t gpu_memory_mib=256;unsigned processor_count=1;std::uint64_t process_affinity=0;
        std::optional<std::array<std::uint64_t,3>> startup_handles;bool startup_suspended=false,startup_detached=false,ignore_ctrl_c=false;std::uint64_t console_identity=0;std::uint32_t console_group=0;ProcessStartupData startup_data;
        bool native_gui = false, live_stdio = false, host_clock = false;
        std::set<std::string_view> seen;
        for (int i = 3; i < argc; i += 2) {
            const std::string_view option = argv[i];
            if (i + 1 >= argc) throw std::invalid_argument("參數缺少值：" + std::string(option));
            if (!seen.insert(option).second && option!="--drive") throw std::invalid_argument("參數重複：" + std::string(option));
            const std::string_view value = argv[i + 1];
            if (option == "--backend") {
                if (comparison) throw std::invalid_argument("compare 固定比對 interpreter 與 jit");
                if (value != "interpreter" && value != "jit") throw std::invalid_argument("未知 backend");
                jit = value == "jit";
            } else if (option == "--dump-jit") dump = value;
            else if (option == "--load-address") config.load_address = GuestAddress{number(value)};
            else if (option == "--entry") config.entry = GuestAddress{number(value)};
            else if (option == "--command-line") pe_options.command_line = utf8_to_utf16(value);
            else if(option=="--process-affinity") {process_affinity=number(value);if(!process_affinity)throw std::invalid_argument("程序 affinity 不可為零");}
            else if(option=="--cpu-count") {
                const auto count=value=="auto"?host_physical_processors():number(value);
                if(!count || count>64 || count>host_physical_processors())throw std::invalid_argument("--cpu-count 超過宿主實體核心數或 64 核心上限");
                processor_count=static_cast<unsigned>(count);
            }
            else if (option == "--clock") {
                if (value != "virtual" && value != "host") throw std::invalid_argument("--clock 需要 virtual 或 host");
                host_clock = value == "host";
            }
            else if (option == "--gui") {
                if (value != "headless" && value != "native") throw std::invalid_argument("--gui 需要 native 或 headless");
                native_gui = value == "native";
            } else if (option == "--gui-snapshot") gui_snapshot = value;
            else if (option == "--fs-root") pe_options.filesystem_root = value;
            else if (option == "--registry-file") pe_options.registry_file = std::filesystem::absolute(value);
            else if (option == "--c-drive") pe_options.c_drive = value;
            else if (option == "--certificate-roots-file") {
                pe_options.certificate_roots_file=certificate_roots_relative_path(value);
                if(pe_options.certificate_roots_file.empty())throw std::invalid_argument("--certificate-roots-file 不接受空路徑");
            }
            else if (option == "--drive") {
                if(value.size()<3 || value[1]!='=')throw std::invalid_argument("--drive 格式為 D=/目錄");
                const char letter=value[0]>='a' && value[0]<='z'?static_cast<char>(value[0]-'a'+'A'):value[0];
                if(letter<'D' || letter>'Z' || !pe_options.drive_mappings.emplace(letter,std::string(value.substr(2))).second)throw std::invalid_argument("--drive 需要不重複的 D-Z 磁碟代號；C 請使用 --c-drive");
            }
            else if (option == "--install-root") pe_options.install_root = value;
            else if (option == "--stdin-file") stdin_file = value;
            else if (option == "--inherit-handles") { if(command!="run-pe")throw std::invalid_argument("繼承清單僅用於 run-pe");inherited_handles=value; }
            else if (option == "--mapping-session") { if(command!="run-pe")throw std::invalid_argument("mapping session 僅用於run-pe");process_session=value; }
            else if (option == "--mapping-session-output") { if(command!="run-pe")throw std::invalid_argument("mapping session output 僅用於 run-pe");mapping_session_output=value; }
            else if (option == "--mapping-session-fd") {const auto descriptor=number(value);if(command!="run-pe" || descriptor<3 || descriptor>65535)throw std::invalid_argument("mapping session fd 無效");process_session_descriptor=static_cast<int>(descriptor);}
            else if (option == "--process-startup-show") {if(command!="run-pe" || number(value)>11)throw std::invalid_argument("子程序顯示模式無效");startup_show=static_cast<std::uint32_t>(number(value));}
            else if(option=="--process-startup-suspended"){if(command!="run-pe" || value!="1")throw std::invalid_argument("子程序暫停參數無效");startup_suspended=true;}
            else if(option=="--process-detached"){if(command!="run-pe" || value!="1")throw std::invalid_argument("子程序 console 分離參數無效");startup_detached=true;}
            else if(option=="--process-console-id"){if(command!="run-pe")throw std::invalid_argument("console 身分只供子程序使用");console_identity=number(value);}
            else if(option=="--process-console-group"){if(command!="run-pe" || number(value)>UINT32_MAX)throw std::invalid_argument("console 群組無效");console_group=static_cast<std::uint32_t>(number(value));}
            else if(option=="--process-ignore-ctrl-c"){if(command!="run-pe" || value!="1")throw std::invalid_argument("console 控制政策無效");ignore_ctrl_c=true;}
            else if(option=="--process-startup-data-fd"){
                const auto descriptor=number(value);if(command!="run-pe" || descriptor<3 || descriptor>65535)throw std::invalid_argument("程序啟動保留資料 fd 無效");
#if !defined(_WIN32)
                struct Close {int fd;~Close(){::close(fd);}} close{static_cast<int>(descriptor)};
#endif
                startup_data=read_process_startup_descriptor(static_cast<int>(descriptor));
            }
            else if(option=="--process-startup-handles"){
                if(command!="run-pe")throw std::invalid_argument("標準 handle 僅用於 run-pe");
                std::array<std::uint64_t,3> handles{};std::size_t begin=0;
                for(unsigned n=0;n<3;++n){const auto end=value.find(',',begin);if((n<2 && end==std::string_view::npos) || (n==2 && end!=std::string_view::npos))throw std::invalid_argument("標準 handle 需要三個值");handles[n]=number(value.substr(begin,end==std::string_view::npos?end:end-begin));begin=end==std::string_view::npos?value.size():end+1;}
                startup_handles=handles;
            }
            else if (option == "--process-directory") { if(command!="run-pe")throw std::invalid_argument("process directory 僅用於run-pe");process_directory=utf8_to_utf16(value); }
            else if (option == "--process-environment-fd") {const auto descriptor=number(value);if(command!="run-pe" || descriptor<3 || descriptor>65535)throw std::invalid_argument("process environment fd 無效");process_environment_descriptor=static_cast<int>(descriptor);}
            else if (option == "--process-token-fd") {const auto descriptor=number(value);if(command!="run-pe" || descriptor<3 || descriptor>65535)throw std::invalid_argument("process token fd 無效");process_token_descriptor=static_cast<int>(descriptor);}
            else if (option == "--process-job-fd") {const auto descriptor=number(value);if(command!="run-pe" || descriptor<3 || descriptor>65535)throw std::invalid_argument("process job fd 無效");process_job_descriptor=static_cast<int>(descriptor);}
            else if (option == "--process-result-fd") { const auto descriptor=number(value);if(command!="run-pe" || descriptor<3 || descriptor>65535)throw std::invalid_argument("process result fd 無效");set_child_result_descriptor(static_cast<int>(descriptor));configure_native_application_role(NativeApplicationRole::Child); }

            else if (option == "--diagnostics-file") {
                if(command!="run-pe")throw std::invalid_argument("--diagnostics-file 僅用於 run-pe");
                diagnostics_file.open(std::filesystem::path(value),std::ios::out|std::ios::trunc);
                if(!diagnostics_file)throw std::invalid_argument("無法建立診斷檔案");
                diagnostics=&diagnostics_file;
            }
            else if (option == "--usage-file") {
                if(command!="run-pe")throw std::invalid_argument("--usage-file 僅用於 run-pe");
                usage_file=std::filesystem::path(value);
            }
            else if(option=="--stdio") {
                if(value!="capture" && value!="live")throw std::invalid_argument("--stdio 需要 capture 或 live");
                live_stdio=value=="live";
            }
            else if (option == "--unresolved-imports") {
                if (value != "reject" && value != "trap") throw std::invalid_argument("--unresolved-imports 需要 reject 或 trap");
                pe_options.trap_unresolved_imports = value == "trap";
            }
            else if (option == "--fs-mode") {
                if (value!="read-only" && value!="read-write") throw std::invalid_argument("--fs-mode 需要 read-only 或 read-write");
                pe_options.writable_files=value=="read-write";
            }
            else if (option == "--image-base") pe_options.image_base = GuestAddress{number(value)};
            else if (option == "--max-instructions") run.max_instructions = number(value);
            else if(option=="--gpu-memory-mib") {
                const auto mib=number(value);
                const auto maximum=host_gpu_memory_limit()/1024/1024;
                if(mib<16 || mib>maximum)throw std::invalid_argument("--gpu-memory-mib 必須介於 16 與 "+std::to_string(maximum)+" MiB；上限為宿主實體記憶體的 50%");
                gpu_memory_mib=static_cast<std::size_t>(mib);
            }
            else if(option=="--memory-limit-mib") {
                const auto mib=number(value);if(mib<64 || mib>8192)throw std::invalid_argument("--memory-limit-mib 必須介於 64 與 8192");
                pe_options.memory_limit=static_cast<std::size_t>(mib)*1024*1024;
            }
            else if (option == "--stack-address") config.stack_address = GuestAddress{number(value)};
            else if (option == "--stack-size") {
                const auto size = number(value);
                if (size > GuestMemory::capacity_limit) throw std::invalid_argument("堆疊超過 256 MiB 客體容量上限");
                config.stack_size = static_cast<std::size_t>(size);
            } else throw std::invalid_argument("未知參數：" + std::string(option));
        }
        if (!jit && seen.contains("--dump-jit")) throw std::invalid_argument("--dump-jit 需要 jit backend");
        if ((!gui_snapshot.empty() && !native_gui) || (comparison && native_gui)) throw std::invalid_argument("GUI 快照需要 --gui native；compare 僅支援 headless GUI");
        if (comparison && pe_options.writable_files) throw std::invalid_argument("compare 不接受 read-write 檔案模式，避免重複修改宿主檔案");
        if(seen.contains("--process-affinity") && command!="run-pe")throw std::invalid_argument("--process-affinity 僅支援 run-pe");
        if(processor_count>1 && (command!="run-pe" || !jit || !host_clock))throw std::invalid_argument("多核心需要 run-pe、JIT 與 --clock host");
        // run-pe 預設以宿主硬體時鐘計時；compare／inspect 與 raw 仍維持可重現的 virtual 時鐘。
        if (!seen.contains("--clock") && command=="run-pe") host_clock = true;
        if (host_clock && (comparison || inspection)) throw std::invalid_argument("--clock host 僅用於 run-pe；compare 需要可重現的 virtual 時鐘");
        if(live_stdio && (comparison || inspection || !stdin_file.empty()))throw std::invalid_argument("--stdio live 僅用於執行，不能與 compare、inspect 或 --stdin-file 併用");
        if (host_files.supported)
            *diagnostics << "host_file_limit inherited=" << host_files.inherited << " current=" << host_files.current
                         << " hard=" << host_files.maximum << '\n';
        *diagnostics<<"cross_architecture host_page_size="<<host_page_size()<<" hardware_tso="<<enter_hardware_tso()<<'\n'
                    <<"apflg enabled="<<hardware_apflg_enabled()<<'\n';diagnostics->flush();
        auto pe_bytes = (command == "run-pe" || inspection || comparison) ?
            read_pe_snapshot(argv[2],comparison?GuestMemory::capacity_limit:std::size_t{2}*1024*1024*1024) : PeFileBytes{};
        auto raw_bytes = pe_bytes ? std::vector<std::uint8_t>{} : read_raw_file(argv[2]);
        auto bytes = pe_bytes ? std::span<const std::uint8_t>{*pe_bytes} : std::span<const std::uint8_t>{raw_bytes};
        const auto release_source = [&] {
            // 載入器已持有映像及匯入資料；不把完整來源（含未映射 overlay）留到客體退出。
            // 僅釋放 CLI 的引用，仍由既有有界 PE 快取決定可重用快照的生命週期。
            bytes={};pe_bytes.reset();std::vector<std::uint8_t>{}.swap(raw_bytes);
        };
        const bool pe = command == "run-pe" || inspection || (comparison && bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z');
        if (pe) {
            for (const auto option : {"--load-address", "--entry", "--stack-address", "--stack-size"})
                if (seen.contains(option)) throw std::invalid_argument(std::string("PE 不接受 raw 選項：") + option);
            pe_options.executable_path=argv[2];
            pe_options.primary_token_descriptor=process_token_descriptor;
            pe_options.defer_imports=startup_suspended;
            // 與 execute() 共用後端選擇；x86 載入器仍維持原 CFG 入口。
            pe_options.resident_translation=jit && !comparison && !inspection;
            if(pe_options.resident_translation && host_clock && !seen.contains("--max-instructions"))
                run.max_instructions=UINT64_MAX;
            auto program = load_pe(bytes, pe_options);
            release_source();
            if(!process_session.empty())program.services.set_mapping_session(process_session,process_session_descriptor);
            *diagnostics<<"cfg_guard fast_path="<<program.services.uses_guard_fast_path()<<'\n';
            if (program.memory.execution_mode() == ExecutionMode::X86) run.sentinel = GuestAddress{0x7ffffff0};
            program.services.set_processor_count(program.memory,processor_count);
            if(process_affinity && !program.services.set_process_affinity(program.state,process_affinity))throw std::invalid_argument("程序 affinity 超出客體核心範圍");
            if (host_clock) program.services.set_host_clock(program.memory);
            if (!stdin_file.empty()) {
                if (inspection) throw std::invalid_argument("inspect-pe 不接受 stdin 輸入");
                std::ifstream file;
                if (stdin_file != "-") { file.open(stdin_file,std::ios::binary); if (!file) throw std::invalid_argument("無法開啟 stdin 輸入檔案"); }
                std::istream& input = stdin_file == "-" ? std::cin : file;
                std::vector<std::uint8_t> content;
                char c;
                while (input.get(c)) { if (content.size() >= WindowsServices::output_limit) throw std::invalid_argument("stdin 快照超過 16 MiB 上限"); content.push_back(static_cast<std::uint8_t>(c)); }
                if (input.bad()) throw std::runtime_error("讀取 stdin 快照失敗");
                program.services.set_standard_input(content);
            }
            if (inspection) {
                std::cout << format_pe(program.metadata);
                for (const auto& name : program.services.unresolved_imports()) std::cout << "unresolved_import=" << name << '\n';
                std::cout << "imports=" << program.metadata.imports.size() << " unresolved=" << program.services.unresolved_imports().size() << '\n';
                return 0;
            }
            if (comparison) return compare(program.state, std::move(program.memory), run, &program.services, dump);
            program.services.set_registry_file(pe_options.registry_file);
            ProcessOptions process_options;process_options.startup_show=startup_show;process_options.registry_file=pe_options.registry_file;
            process_options.startup_handles=startup_handles;process_options.startup_suspended=startup_suspended;process_options.startup_detached=startup_detached;
            process_options.console_identity=console_identity;process_options.console_group=console_group;process_options.ignore_ctrl_c=ignore_ctrl_c;
            process_options.startup_data=std::move(startup_data);
            process_options.engine=argv[0];process_options.filesystem_root=pe_options.filesystem_root;process_options.c_drive=pe_options.c_drive;process_options.drive_mappings=pe_options.drive_mappings;process_options.install_root=pe_options.install_root;
            process_options.certificate_roots_file=pe_options.certificate_roots_file;
            process_options.processor_count=processor_count;process_options.jit=jit;process_options.native_gui=native_gui;process_options.host_clock=host_clock;process_options.writable_files=pe_options.writable_files;process_options.trap_unresolved=pe_options.trap_unresolved_imports;
            process_options.max_instructions=run.max_instructions;process_options.memory_limit_mib=program.memory.limit()/1024/1024;process_options.gpu_memory_mib=gpu_memory_mib;
            program.services.configure_processes(std::move(process_options), program.memory);
            if(process_session_descriptor>=0 && process_session.empty())throw std::invalid_argument("mapping session fd 需要命名空間路徑");
            if(!mapping_session_output.empty()) {
                const auto temporary=mapping_session_output.string()+".tmp";
                {std::ofstream output(temporary,std::ios::binary);output<<program.services.mapping_session()<<'\n';output.close();if(!output)throw std::runtime_error("無法發布 Windows 工作階段路徑");}
                std::filesystem::rename(temporary,mapping_session_output);
            }
#if !defined(_WIN32)
            if(process_session_descriptor>=0)::close(process_session_descriptor);
#endif
            program.services.configure_handle_transfer();
            if(process_environment_descriptor>=0){program.services.import_guest_environment(program.memory,program.state,process_environment_descriptor);
#if !defined(_WIN32)
                ::close(process_environment_descriptor);
#endif
            }
            if(process_token_descriptor>=0){
#if !defined(_WIN32)
                ::close(process_token_descriptor);
#endif
            }
            if(process_job_descriptor>=0){
                const auto error=program.services.import_process_job(process_job_descriptor);
#if !defined(_WIN32)
                ::close(process_job_descriptor);
#endif
                report_child_job_status(error);if(error){program.services.report_process_accounting();report_child_result(error,true);return 0;}
            }
            if(!startup_suspended && !process_directory.empty())program.services.set_process_directory(std::move(process_directory));
            if(program.memory.execution_mode()==ExecutionMode::X86 && program.state.fs_base)program.memory.store(GuestAddress{program.state.fs_base+0x20},32,program.services.process_id());
            if(program.state.gs_base)program.memory.store(GuestAddress{program.state.gs_base+0x40},64,program.services.process_id());

            if(live_stdio && !startup_detached) {
#if !defined(_WIN32)
                std::signal(SIGPIPE,SIG_IGN);
#endif
                program.services.set_live_standard_io();
            }
            if(!inherited_handles.empty())program.services.import_process_handles(inherited_handles);
            program.services.import_crt_startup(program.state,program.memory);
            configure_gpu_memory(gpu_memory_mib*1024*1024);
            ResourceUsageReporter resource_usage(usage_file);
            run.services = &program.services;
            startup_profile::mark("child_setup_ready");
            report_child_startup_ready();
            if(startup_suspended)program.services.after_instructions(program.state,program.memory,0);
            // CreateProcess 只需程序與初始執行緒已建立，不等子程序初始化；父程序在就緒後還要等子程序
            // 處理恢復執行緒的遠端請求。原生 GUI 後端（約 0.1 秒）因此改在暫停等待結束之後、
            // PE 載入完成與客體執行之前建立：暫停期間不執行客體碼也不會有視窗，父程序不再為它阻塞。
            // 初始化失敗時子程序隨後結束，與 Windows 程序初始化失敗的語義一致。
            if (native_gui){startup_profile::Scope measurement("startup","native_gui_init",true);program.services.enable_native_gui(gui_snapshot);}
            if(startup_suspended && !process_directory.empty())program.services.set_process_directory(std::move(process_directory));
            complete_pe_load(program);
            *diagnostics << format_pe(program.metadata);
            *diagnostics << "unresolved_imports=" << program.services.unresolved_imports().size() << '\n';
            *diagnostics << "memory_limit_mib=" << program.memory.limit()/1024/1024 << '\n';
            *diagnostics << "clock=" << (host_clock ? "host" : "virtual") << '\n';
            const auto result = execute(jit, program.state, program.memory, run, dump, *diagnostics, argv[2]);
            if (!program.services.gui_state().classes.empty()) {
                const auto& gui = program.services.gui_state();
                *diagnostics << "gui=" << (native_gui ? "native" : "headless") << " dispatched=" << gui.dispatched << " paints=" << gui.paints << " destroyed=" << gui.destroyed << '\n';
            }
            std::cout << program.services.standard_output();
            std::cerr << program.services.standard_error();
            *diagnostics << format_result(result, program.state);
            *diagnostics << program.services.video_diagnostics();
            *diagnostics << program.services.opengl_diagnostics();
            *diagnostics << program.services.dsound_diagnostics();
            *diagnostics << program.services.d3d9_diagnostics();
            *diagnostics << program.services.process_diagnostics();
            *diagnostics << gpu_memory_diagnostics();
            *diagnostics << program.services.module_diagnostics();
            *diagnostics << program.services.native_library_diagnostics();
            program.services.report_process_accounting();
            const auto child_exit=result.exit_code.value_or(result.reason==StopReason::Returned?0U:
                result.reason==StopReason::MemoryFault?0xc0000005U:
                result.reason==StopReason::InvalidInstruction?0xc000001dU:0xc0000001U);
            program.services.record_job_exit(child_exit);
            report_child_result(child_exit,result.reason==StopReason::Exited || result.reason==StopReason::Returned);
            if (result.reason == StopReason::Exited) return static_cast<int>(*result.exit_code & 255U);
            return result.reason == StopReason::Returned ? 0 : 1;
        }
        if (seen.contains("--drive") || seen.contains("--gpu-memory-mib") || seen.contains("--clock") || seen.contains("--memory-limit-mib") || seen.contains("--stdio") || seen.contains("--fs-mode") || seen.contains("--stdin-file") || seen.contains("--c-drive") || seen.contains("--install-root") || seen.contains("--unresolved-imports") || seen.contains("--gui") || seen.contains("--gui-snapshot") || seen.contains("--image-base") || seen.contains("--command-line") || seen.contains("--fs-root") || seen.contains("--certificate-roots-file")) throw std::invalid_argument("PE 選項不能用於 raw 程式");
        for (const auto required : {"--load-address", "--entry"})
            if (!seen.contains(required)) throw std::invalid_argument(std::string("raw 檔必須指定 ") + required);
        if(jit && !comparison && !seen.contains("--max-instructions"))run.max_instructions=UINT64_MAX;
        auto program = prepare_raw(bytes, config);
        release_source();
        if (comparison) return compare(program.state, std::move(program.memory), run, nullptr, dump);
        const auto result = execute(jit, program.state, program.memory, run, dump, std::cout);
        std::cout << format_result(result, program.state);
        return result.reason == StopReason::Returned ? 0 : 1;
    } catch (const PeDependencyError& error) {
        const auto status=error.exit_status();report_child_startup_failure(error.win32_error);report_child_result(status,true);
        *diagnostics << "guest_loader_error windows_error=" << error.win32_error << " exit_code=" << status << "：" << error.what() << '\n';
        return 2;
    } catch (const PlatformUnavailable& error) {
        report_child_result(0,false);
        *diagnostics << "platform_unavailable：" << error.what() << '\n'; return 77;
    } catch (const std::invalid_argument& error) {
        report_child_result(0,false);
        *diagnostics << "輸入錯誤：" << error.what() << '\n'; return 2;
    } catch (const MemoryFault& error) {
        report_child_result(0,false);
        *diagnostics << "載入錯誤：" << error.what() << " guest_address=0x" << std::hex << error.address.value
            << " access=" << access_name(error.access) << '\n'; return 2;
    } catch (const std::exception& error) {
        report_child_result(0,false);
        *diagnostics << "internal_error：" << error.what() << '\n'; return 1;
    }
}
