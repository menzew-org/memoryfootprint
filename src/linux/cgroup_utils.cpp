/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fail-closed cgroup-v2 subtree enumeration.
 */

#include "cgroup_utils.h"

#include <fstream>
#include <limits>

namespace memftprt::linux_cgroup {
namespace {

bool add_processes(const std::filesystem::path& directory, std::set<pid_t>* members,
                   std::string* reason) {
    const std::filesystem::path membership = directory / "cgroup.procs";
    std::ifstream processes(membership);
    if (!processes) {
        *reason = "could not read " + membership.string();
        return false;
    }

    long value = 0;
    while (processes >> value) {
        if (value <= 0 || value > std::numeric_limits<pid_t>::max()) {
            *reason = "invalid process identifier in " + membership.string();
            return false;
        }
        members->insert(static_cast<pid_t>(value));
    }
    if (!processes.eof()) {
        *reason = "could not parse " + membership.string();
        return false;
    }
    return true;
}

}  // namespace

bool read_subtree_processes(const std::filesystem::path& cgroup_path,
                            std::set<pid_t>* members, std::string* reason) {
    members->clear();
    if (!add_processes(cgroup_path, members, reason)) return false;

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        cgroup_path, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        *reason = "could not enumerate cgroup subtree " + cgroup_path.string() + ": " +
                  error.message();
        return false;
    }

    while (iterator != end) {
        const bool is_symlink = iterator->is_symlink(error);
        if (error) {
            *reason = "could not inspect cgroup subtree entry " + iterator->path().string() +
                      ": " + error.message();
            return false;
        }
        if (is_symlink) {
            *reason = "unexpected symbolic link in cgroup subtree: " +
                      iterator->path().string();
            return false;
        }
        const bool is_directory = iterator->is_directory(error);
        if (error) {
            *reason = "could not inspect cgroup subtree entry " + iterator->path().string() +
                      ": " + error.message();
            return false;
        }
        if (is_directory && !add_processes(iterator->path(), members, reason)) return false;

        iterator.increment(error);
        if (error) {
            *reason = "could not enumerate cgroup subtree " + cgroup_path.string() + ": " +
                      error.message();
            return false;
        }
    }
    return true;
}

}  // namespace memftprt::linux_cgroup
