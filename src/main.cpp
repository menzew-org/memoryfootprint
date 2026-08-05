/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Process entry point: parse the CLI, dispatch capture, and normalize errors.
 */

#include <cstdio>
#include <cstring>

#include "capture.h"
#include "cli.h"

int main(int argc, char** argv) {
    try {
        const memftprt::cli::Result parsed = memftprt::cli::parse(argc, argv);
        if (parsed.action == memftprt::cli::Action::help) {
            memftprt::cli::print_help(stdout, parsed.command_line.program_name.c_str());
            return 0;
        }
        if (parsed.action == memftprt::cli::Action::version) {
            std::printf("memftprt %s\n", memftprt::cli::VERSION);
            return 0;
        }
        return memftprt::run_capture(parsed.command_line, argv);
    } catch (const memftprt::cli::Error& error) {
        const char* program_name = "memftprt";
        if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
            // Errors can occur before the parser returns its normalized
            // CommandLine, so derive the friendly executable name here too.
            const char* slash = std::strrchr(argv[0], '/');
            program_name = slash == nullptr ? argv[0] : slash + 1;
        }
        memftprt::cli::print_error(stderr, program_name, error.what());
        return 2;
    }
}
