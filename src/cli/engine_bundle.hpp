#pragma once

#if defined(X64ARM_CLI_BUNDLE_PATH) && defined(__APPLE__) && defined(__aarch64__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace x64arm::cli {
using EngineUuid = std::array<unsigned char, 16>;

inline std::optional<EngineUuid> engine_uuid(const mach_header_64& header, std::span<const char> commands) {
    if (header.magic != MH_MAGIC_64 || commands.size() != header.sizeofcmds ||
        header.ncmds > commands.size() / sizeof(load_command))
        return {};
    std::size_t offset = 0;
    for (std::uint32_t n = 0; n < header.ncmds; ++n) {
        if (commands.size() - offset < sizeof(load_command)) return {};
        load_command command{};
        std::memcpy(&command, commands.data() + offset, sizeof(command));
        if (command.cmdsize < sizeof(command) || command.cmdsize > commands.size() - offset) return {};
        if (command.cmd == LC_UUID) {
            if (command.cmdsize != sizeof(uuid_command)) return {};
            uuid_command uuid{};
            std::memcpy(&uuid, commands.data() + offset, sizeof(uuid));
            EngineUuid result{};
            std::memcpy(result.data(), uuid.uuid, result.size());
            return result;
        }
        offset += command.cmdsize;
    }
    return {};
}

inline void enter_authorized_bundle(char** argv) {
    if (const auto* setting = std::getenv("X64ARM_CROSS_ARCH"); setting && std::string_view(setting) == "0") return;
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) throw std::runtime_error("無法取得 CLI 啟動路徑");
    const auto executable = std::filesystem::canonical(path.data());
    const auto bundle = executable.parent_path() / X64ARM_CLI_BUNDLE_PATH;
    // 單獨複製的 CLI 仍保留既有軟體排序路徑；bundle 內的引擎不再轉送。
    if (!std::filesystem::exists(bundle) || std::filesystem::equivalent(executable, bundle)) return;

    const auto* image = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    if (!image || image->magic != MH_MAGIC_64 || image->sizeofcmds > 1024 * 1024)
        throw std::runtime_error("無法辨識目前 CLI 的 Mach-O 版本");
    const auto current = engine_uuid(*image, {reinterpret_cast<const char*>(image + 1), image->sizeofcmds});
    std::ifstream input(bundle, std::ios::binary);
    mach_header_64 header{};
    if (!input.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != MH_MAGIC_64 ||
        header.sizeofcmds > 1024 * 1024)
        throw std::runtime_error("無法辨識已授權引擎的 Mach-O 版本");
    std::vector<char> commands(header.sizeofcmds);
    if (!input.read(commands.data(), static_cast<std::streamsize>(commands.size())) || !current ||
        current != engine_uuid(header, commands))
        throw std::runtime_error("CLI 與授權 bundle 版本不同，請重新建置 x64arm");
    input.close();

    // exec 保留 PID、引數、環境及繼承的描述元；受限制授權仍由完整 bundle 提供。
    ::execv(bundle.c_str(), argv);
    throw std::runtime_error("無法啟動已授權 CLI：" + std::string(std::strerror(errno)));
}
}
#else
namespace x64arm::cli {
inline void enter_authorized_bundle(char**) {}
}
#endif
