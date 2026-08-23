/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform-neutral capture configuration shared by the CLI and backends.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace memftprt {

enum class FreezeMode {
    automatic,
    cgroup,
    signals,
};

enum class ReportFormat {
    tsv,
    json,
    html,
    all,
};

struct CaptureConfig {
    int debug_level = 0;
    int wait_time = -1;
    unsigned quiet_window_seconds = 3;
    unsigned quiescence_timeout_seconds = 300;
    bool quiet = false;
    FreezeMode freeze_mode = FreezeMode::automatic;
    bool jcmd_enabled = true;
    bool enable_jvm_nmt = false;
    bool mappings_enabled = false;
    unsigned working_set_seconds = 0;
    std::string launch_cgroup;
    std::filesystem::path output_directory = ".";
    ReportFormat report_format = ReportFormat::tsv;
};

struct CommandLine {
    CaptureConfig config;
    std::string program_name = "memftprt";
    std::int64_t target_pid = -1;
    int command_index = -1;
};

}  // namespace memftprt
