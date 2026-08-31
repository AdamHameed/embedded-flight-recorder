#include "flight_recorder/platform_info.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <sys/utsname.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#endif

#ifndef FLIGHT_RECORDER_BUILD_TYPE
#define FLIGHT_RECORDER_BUILD_TYPE "unknown"
#endif

#ifndef FLIGHT_RECORDER_SOURCE_DIR
#define FLIGHT_RECORDER_SOURCE_DIR "."
#endif

namespace flight_recorder {
namespace {

std::string trim(std::string value) {
    const auto last = value.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) {
        return {};
    }
    value.erase(last + 1);
    const auto first = value.find_first_not_of(" \t\r\n");
    return value.substr(first);
}

std::string run_command(const std::string& command) {
    std::array<char, 256> buffer {};
    std::string output;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return {};
    }
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    if (::pclose(pipe) != 0) {
        return {};
    }
    return trim(output);
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
}

std::string compiler_name() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

std::string kernel_name() {
    struct utsname details {};
    if (::uname(&details) != 0) {
        return "unknown";
    }
    return std::string(details.sysname) + " " + details.release + " " + details.machine;
}

std::string cpu_name() {
#if defined(__APPLE__)
    std::size_t length = 0;
    if (::sysctlbyname("machdep.cpu.brand_string", nullptr, &length, nullptr, 0) == 0 && length > 1) {
        std::string value(length, '\0');
        if (::sysctlbyname("machdep.cpu.brand_string", value.data(), &length, nullptr, 0) == 0) {
            value.resize(length - 1);
            return value;
        }
    }
    if (::sysctlbyname("hw.model", nullptr, &length, nullptr, 0) == 0 && length > 1) {
        std::string value(length, '\0');
        if (::sysctlbyname("hw.model", value.data(), &length, nullptr, 0) == 0) {
            value.resize(length - 1);
            return value;
        }
    }
#elif defined(__linux__)
    std::ifstream input("/proc/cpuinfo");
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find(':');
        if (separator != std::string::npos &&
            (line.compare(0, 10, "model name") == 0 || line.compare(0, 8, "Hardware") == 0)) {
            return trim(line.substr(separator + 1));
        }
    }
#endif
    return "unknown";
}

std::string filesystem_name(const std::string& path) {
    struct statfs details {};
    if (::statfs(path.c_str(), &details) != 0) {
        return "unknown";
    }
#if defined(__APPLE__)
    return details.f_fstypename;
#elif defined(__linux__)
    std::ostringstream value;
    value << "type=0x" << std::hex << static_cast<unsigned long>(details.f_type);
    return value.str();
#else
    return "unknown";
#endif
}

std::string storage_device_name(const std::string& path) {
#if defined(__APPLE__)
    struct statfs details {};
    if (::statfs(path.c_str(), &details) == 0) {
        return details.f_mntfromname;
    }
#elif defined(__linux__)
    struct stat details {};
    if (::stat(path.c_str(), &details) == 0) {
        return std::to_string(::major(details.st_dev)) + ":" +
               std::to_string(::minor(details.st_dev));
    }
#endif
    return "unknown";
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << '?';
            } else {
                output << character;
            }
        }
    }
    return output.str();
}

}  // namespace

PlatformInfo collect_platform_info(const std::string& filesystem_path) {
    const std::string source_dir = FLIGHT_RECORDER_SOURCE_DIR;
    const std::string git_prefix = "git -C " + shell_quote(source_dir) + " ";
    PlatformInfo info;
    info.compiler = compiler_name();
    info.build_type = FLIGHT_RECORDER_BUILD_TYPE;
    info.os_kernel = kernel_name();
    info.cpu = cpu_name();
    info.filesystem = filesystem_name(filesystem_path);
    info.storage_device = storage_device_name(filesystem_path);
    info.git_revision = run_command(git_prefix + "rev-parse --short HEAD 2>/dev/null");
    const std::string status = run_command(git_prefix + "status --porcelain --untracked-files=normal 2>/dev/null");
    info.dirty_worktree = info.git_revision.empty() || !status.empty();
    return info;
}

std::string platform_info_json(const PlatformInfo& info) {
    std::ostringstream output;
    output << '{'
           << "\"compiler\":\"" << json_escape(info.compiler) << "\","
           << "\"build_type\":\"" << json_escape(info.build_type) << "\","
           << "\"os_kernel\":\"" << json_escape(info.os_kernel) << "\","
           << "\"cpu\":\"" << json_escape(info.cpu) << "\","
           << "\"filesystem\":\"" << json_escape(info.filesystem) << "\","
           << "\"storage_device\":\"" << json_escape(info.storage_device) << "\","
           << "\"git_revision\":\"" << json_escape(info.git_revision) << "\","
           << "\"dirty_worktree\":" << (info.dirty_worktree ? "true" : "false")
           << '}';
    return output.str();
}

}  // namespace flight_recorder
