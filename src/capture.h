/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Narrow entry point implemented by the active platform capture backend.
 */
#pragma once

#include "config.h"

namespace memftprt {

int run_capture(const CommandLine& command_line, char** argv);

}  // namespace memftprt
