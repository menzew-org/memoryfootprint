/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux referenced-working-set primitives implementation.
 */

#include "working_set.h"

#include <cerrno>
#include <cstring>
#include <tuple>

#include <fcntl.h>
#include <unistd.h>

namespace memftprt::linux_working_set {

bool MappingIdentity::operator<(const MappingIdentity& other) const {
    return std::tie(start_address, end_address, offset, inode, permissions, device, pathname) <
           std::tie(other.start_address, other.end_address, other.offset, other.inode,
                    other.permissions, other.device, other.pathname);
}

bool MappingIdentity::operator==(const MappingIdentity& other) const {
    return start_address == other.start_address && end_address == other.end_address &&
           offset == other.offset && inode == other.inode && permissions == other.permissions &&
           device == other.device && pathname == other.pathname;
}

bool clear_referenced_bits(pid_t pid, std::string* reason) {
    const std::string path = "/proc/" + std::to_string(pid) + "/clear_refs";
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (reason != nullptr) *reason = "open " + path + " failed: " + std::strerror(errno);
        return false;
    }
    constexpr char value[] = "1\n";
    const ssize_t written = write(fd, value, sizeof(value) - 1);
    const int write_errno = errno;
    const int close_result = close(fd);
    if (written != static_cast<ssize_t>(sizeof(value) - 1)) {
        if (reason != nullptr) {
            *reason = written < 0 ? "write " + path + " failed: " + std::strerror(write_errno)
                                  : "short write to " + path;
        }
        return false;
    }
    if (close_result < 0) {
        if (reason != nullptr) *reason = "close " + path + " failed: " + std::strerror(errno);
        return false;
    }
    return true;
}

std::set<MappingIdentity> mapping_identities(const linux_procfs::SmapsDetail& detail) {
    std::set<MappingIdentity> result;
    for (const linux_procfs::SmapsMapping& mapping : detail.mappings) {
        result.insert(MappingIdentity {mapping.start_address, mapping.end_address, mapping.offset,
                                       mapping.inode, mapping.permissions, mapping.device,
                                       mapping.pathname});
    }
    return result;
}

}  // namespace memftprt::linux_working_set
