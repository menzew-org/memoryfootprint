/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Command-line interface contract. Parsing is kept independent of capture so
 * every platform exposes the same user-facing options and validation.
 */
#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>

#include "config.h"

namespace memftprt::cli {

constexpr const char* VERSION = "0.1.0";

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

enum class Action {
    run,
    help,
    version,
};

struct Result {
    Action action = Action::run;
    CommandLine command_line;
};

Result parse(int argc, char** argv);
void print_help(FILE* stream, const char* program_name);
void print_error(FILE* stream, const char* program_name, const std::string& message);

}  // namespace memftprt::cli
