#pragma once

#include <string>

namespace flight_recorder {

struct PlatformInfo {
    std::string compiler;
    std::string build_type;
    std::string os_kernel;
    std::string cpu;
    std::string filesystem;
    std::string storage_device;
    std::string git_revision;
    bool dirty_worktree {true};
};

PlatformInfo collect_platform_info(const std::string& filesystem_path);
std::string platform_info_json(const PlatformInfo& info);

}  // namespace flight_recorder
