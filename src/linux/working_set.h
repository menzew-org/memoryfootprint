/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux referenced-working-set primitives.
 *
 * Capture orchestration owns freeze/resume policy. This module only mutates
 * clear_refs and builds stable identities for already-frozen smaps records.
 */
#pragma once

#include <cstdint>
#include <set>
#include <string>

#include <sys/types.h>

#include "proc_memory.h"

namespace memftprt::linux_working_set {

struct MappingIdentity {
    std::uint64_t start_address = 0;
    std::uint64_t end_address = 0;
    std::uint64_t offset = 0;
    std::uint64_t inode = 0;
    std::string permissions;
    std::string device;
    std::string pathname;

    bool operator<(const MappingIdentity& other) const;
    bool operator==(const MappingIdentity& other) const;
};

// Value 1 clears referenced/accessed state for all pages of the process.
bool clear_referenced_bits(pid_t pid, std::string* reason);

std::set<MappingIdentity> mapping_identities(
    const linux_procfs::SmapsDetail& detail);

}  // namespace memftprt::linux_working_set
