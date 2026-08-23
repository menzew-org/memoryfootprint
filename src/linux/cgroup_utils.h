/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Strict cgroup-v2 subtree inspection used before and after freezing.
 */
#pragma once

#include <filesystem>
#include <set>
#include <string>

#include <sys/types.h>

namespace memftprt::linux_cgroup {

// Read every cgroup.procs file in a subtree. Any unreadable directory, entry,
// or membership file is a hard failure: callers use this result to prove that
// freezing the subtree cannot affect an unseen process.
bool read_subtree_processes(const std::filesystem::path& cgroup_path,
                            std::set<pid_t>* members, std::string* reason);

}  // namespace memftprt::linux_cgroup
