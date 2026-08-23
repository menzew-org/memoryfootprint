/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform-neutral command-line parsing and help text.
 */

#include "cli.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace memftprt::cli {
namespace {

long parse_positive(const char* value, const char* option) {
    errno = 0;
    char* end = nullptr;
    const long result = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || result < 1) {
        throw Error(std::string(option) + " requires a positive integer, got '" + value + "'");
    }
    return result;
}

ReportFormat parse_report_format(const char* value) {
    if (std::strcmp(value, "tsv") == 0) return ReportFormat::tsv;
    if (std::strcmp(value, "json") == 0) return ReportFormat::json;
    if (std::strcmp(value, "html") == 0) return ReportFormat::html;
    if (std::strcmp(value, "all") == 0) return ReportFormat::all;
    throw Error(std::string("unknown report format '") + value +
                "'; expected tsv, json, html, or all");
}

}  // namespace

void print_help(FILE* stream, const char* program_name) {
    std::fprintf(stream, "memftprt %s - consistent process-tree memory capture\n\n", VERSION);
    std::fprintf(stream, "Usage:\n");
    std::fprintf(stream, "  %s [options] -- command [args...]\n", program_name);
    std::fprintf(stream, "  %s [options] --pid PID\n\n", program_name);
    std::fprintf(stream, "Capture timing:\n");
    std::fprintf(stream, "  -q, --stable-window SEC  required stable window (default: 3)\n");
    std::fprintf(stream, "  -m, --timeout SEC        maximum quiescence wait (default: 300)\n");
    std::fprintf(stream, "  -t, --delay SEC          use a fixed delay instead of quiescence\n\n");
    std::fprintf(stream, "Target and consistency:\n");
    std::fprintf(stream, "  -p, --pid PID            attach to an existing process\n");
    std::fprintf(stream, "      --freeze MODE        auto, cgroup, or signals (default: auto)\n");
    std::fprintf(stream, "      --cgroup PATH        launch in an empty delegated cgroup v2\n\n");
    std::fprintf(stream, "Runtime capture:\n");
    std::fprintf(stream, "      --enable-jvm-nmt     enable HotSpot NMT for a launched JVM\n");
    std::fprintf(stream, "      --no-jcmd            disable automatic JVM NMT collection\n\n");
    std::fprintf(stream, "Memory detail:\n");
    std::fprintf(stream, "      --mappings           capture and classify every memory mapping\n");
    std::fprintf(stream, "      --working-set SEC    measure pages referenced during an interval\n\n");
    std::fprintf(stream, "Output and diagnostics:\n");
    std::fprintf(stream, "  -o, --output-dir DIR     write artifacts to DIR (default: current directory)\n");
    std::fprintf(stream, "  -f, --format FORMAT      tsv, json, html, or all (default: tsv)\n");
    std::fprintf(stream, "      --quiet              emit nothing on successful capture\n");
    std::fprintf(stream, "  -v, --verbose            show capture decisions; repeat for more detail\n");
    std::fprintf(stream, "  -h, --help               show this help and exit\n");
    std::fprintf(stream, "  -V, --version            show version and exit\n\n");
    std::fprintf(stream, "Examples:\n");
    std::fprintf(stream, "  %s -o captures --format all -- ./server --port 8080\n", program_name);
    std::fprintf(stream, "  %s --pid 1234 --stable-window 5\n", program_name);
    std::fprintf(stream, "  %s --enable-jvm-nmt -- java -jar app.jar\n", program_name);
    std::fprintf(stream, "  %s --working-set 10 --format all -- ./server\n", program_name);
}

void print_error(FILE* stream, const char* program_name, const std::string& message) {
    std::fprintf(stream, "%s: error: %s\nTry '%s --help' for usage.\n", program_name,
                 message.c_str(), program_name);
}

Result parse(int argc, char** argv) {
    Result result;
    if (argv != nullptr && argv[0] != nullptr) {
        const char* slash = std::strrchr(argv[0], '/');
        result.command_line.program_name = slash == nullptr ? argv[0] : slash + 1;
    }
    if (argc < 2 || argv == nullptr || argv[0] == nullptr) {
        throw Error("a command or --pid PID is required");
    }

    CaptureConfig& config = result.command_line.config;
    int index = 1;
    while (index < argc && argv[index] != nullptr && argv[index][0] == '-') {
        const char* option = argv[index];
        if (std::strcmp(option, "--") == 0) {
            ++index;
            break;
        }
        if (std::strcmp(option, "-h") == 0 || std::strcmp(option, "--help") == 0) {
            result.action = Action::help;
            return result;
        }
        if (std::strcmp(option, "-V") == 0 || std::strcmp(option, "--version") == 0) {
            result.action = Action::version;
            return result;
        }
        if (std::strcmp(option, "-p") == 0 || std::strcmp(option, "--pid") == 0) {
            if (++index >= argc) throw Error(std::string(option) + " requires a PID");
            result.command_line.target_pid = parse_positive(argv[index++], option);
        } else if (std::strcmp(option, "-t") == 0 || std::strcmp(option, "--delay") == 0) {
            if (++index >= argc) {
                throw Error(std::string(option) + " requires a duration in seconds");
            }
            const long value = parse_positive(argv[index++], option);
            if (value > std::numeric_limits<int>::max()) {
                throw Error("delay is outside the supported range");
            }
            config.wait_time = static_cast<int>(value);
        } else if (std::strcmp(option, "-q") == 0 ||
                   std::strcmp(option, "--stable-window") == 0) {
            if (++index >= argc) {
                throw Error(std::string(option) + " requires a duration in seconds");
            }
            const unsigned long value =
                static_cast<unsigned long>(parse_positive(argv[index++], option));
            if (value > std::numeric_limits<unsigned>::max()) {
                throw Error("stable window is outside the supported range");
            }
            config.quiet_window_seconds = static_cast<unsigned>(value);
        } else if (std::strcmp(option, "-m") == 0 ||
                   std::strcmp(option, "--timeout") == 0) {
            if (++index >= argc) {
                throw Error(std::string(option) + " requires a duration in seconds");
            }
            const unsigned long value =
                static_cast<unsigned long>(parse_positive(argv[index++], option));
            if (value > std::numeric_limits<unsigned>::max()) {
                throw Error("timeout is outside the supported range");
            }
            config.quiescence_timeout_seconds = static_cast<unsigned>(value);
        } else if (std::strcmp(option, "-o") == 0 ||
                   std::strcmp(option, "--output-dir") == 0) {
            if (++index >= argc) throw Error(std::string(option) + " requires a directory");
            config.output_directory = argv[index++];
        } else if (std::strcmp(option, "-f") == 0 ||
                   std::strcmp(option, "--format") == 0) {
            if (++index >= argc) throw Error(std::string(option) + " requires a format");
            config.report_format = parse_report_format(argv[index++]);
        } else if (std::strcmp(option, "--quiet") == 0) {
            config.quiet = true;
            ++index;
        } else if (std::strcmp(option, "-v") == 0 ||
                   std::strcmp(option, "--verbose") == 0) {
            ++config.debug_level;
            ++index;
        } else if (std::strcmp(option, "--freeze") == 0) {
            if (++index >= argc) throw Error("--freeze requires auto, cgroup, or signals");
            if (std::strcmp(argv[index], "auto") == 0) {
                config.freeze_mode = FreezeMode::automatic;
            } else if (std::strcmp(argv[index], "cgroup") == 0) {
                config.freeze_mode = FreezeMode::cgroup;
            } else if (std::strcmp(argv[index], "signals") == 0) {
                config.freeze_mode = FreezeMode::signals;
            } else {
                throw Error(std::string("unknown freeze mode '") + argv[index] +
                            "'; expected auto, cgroup, or signals");
            }
            ++index;
        } else if (std::strcmp(option, "--cgroup") == 0) {
            if (++index >= argc) throw Error("--cgroup requires a delegated cgroup v2 path");
            config.launch_cgroup = argv[index++];
        } else if (std::strcmp(option, "--no-jcmd") == 0) {
            config.jcmd_enabled = false;
            ++index;
        } else if (std::strcmp(option, "--enable-jvm-nmt") == 0) {
            config.enable_jvm_nmt = true;
            ++index;
        } else if (std::strcmp(option, "--mappings") == 0) {
            config.mappings_enabled = true;
            ++index;
        } else if (std::strcmp(option, "--working-set") == 0) {
            if (++index >= argc) {
                throw Error("--working-set requires a duration in seconds");
            }
            const unsigned long value =
                static_cast<unsigned long>(parse_positive(argv[index++], "--working-set"));
            if (value > std::numeric_limits<unsigned>::max()) {
                throw Error("working-set window is outside the supported range");
            }
            config.working_set_seconds = static_cast<unsigned>(value);
            config.mappings_enabled = true;
        } else {
            throw Error(std::string("unknown option '") + option + "'");
        }
    }

    if (result.command_line.target_pid != -1) {
        if (result.command_line.target_pid > std::numeric_limits<int>::max()) {
            throw Error("PID is outside the supported range");
        }
        if (config.enable_jvm_nmt || !config.launch_cgroup.empty()) {
            throw Error("--enable-jvm-nmt and --cgroup are launch-only options");
        }
        if (index != argc) throw Error("a command cannot be combined with --pid");
    } else {
        if (index >= argc) throw Error("a command or --pid PID is required");
        if (!config.launch_cgroup.empty()) {
            if (config.freeze_mode == FreezeMode::signals) {
                throw Error("--cgroup conflicts with --freeze signals");
            }
            config.freeze_mode = FreezeMode::cgroup;
        }
        result.command_line.command_index = index;
    }
    return result;
}

}  // namespace memftprt::cli
