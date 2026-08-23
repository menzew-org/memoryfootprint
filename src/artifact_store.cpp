/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Secure artifact publication implementation for local filesystems.
 */

#include "artifact_store.h"

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace memftprt {

ArtifactStore::ArtifactStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

ArtifactStore::~ArtifactStore() {
    release_lock();
    if (directory_fd_ >= 0) close(directory_fd_);
}

bool ArtifactStore::prepare(std::string* reason) {
    if (directory_fd_ >= 0) {
        close(directory_fd_);
        directory_fd_ = -1;
    }
    std::error_code error;
    directory_ = std::filesystem::absolute(directory_, error).lexically_normal();
    if (error) {
        *reason = "could not resolve output directory " + directory_.string() + ": " +
                  error.message();
        return false;
    }

    std::size_t created_directory_count = 0;
    std::filesystem::path existing_ancestor = directory_;
    while (!std::filesystem::exists(existing_ancestor, error)) {
        if (error) {
            *reason = "could not inspect output directory " + existing_ancestor.string() + ": " +
                      error.message();
            return false;
        }
        ++created_directory_count;
        const std::filesystem::path parent = existing_ancestor.parent_path();
        if (parent.empty() || parent == existing_ancestor) {
            *reason = "could not find an existing parent for output directory " +
                      directory_.string();
            return false;
        }
        existing_ancestor = parent;
    }
    if (error) {
        *reason = "could not inspect output directory " + existing_ancestor.string() + ": " +
                  error.message();
        return false;
    }

    if (created_directory_count != 0) {
        (void)std::filesystem::create_directories(directory_, error);
        if (error) {
            *reason = "could not create output directory " + directory_.string() + ": " +
                      error.message();
            return false;
        }
    }
    if (error || !std::filesystem::is_directory(directory_, error)) {
        *reason = "output path is not a directory: " + directory_.string();
        return false;
    }
    directory_ = std::filesystem::canonical(directory_, error);
    if (error || access(directory_.c_str(), W_OK | X_OK) < 0) {
        *reason = "output directory is not writable: " + directory_.string();
        return false;
    }
    directory_fd_ = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd_ < 0) {
        *reason = "could not open output directory " + directory_.string() + ": " +
                  std::strerror(errno);
        return false;
    }
    if (created_directory_count != 0 &&
        !sync_created_directory_chain(created_directory_count, reason)) {
        close(directory_fd_);
        directory_fd_ = -1;
        return false;
    }
    return true;
}

bool ArtifactStore::acquire_lock(std::int64_t target_pid, std::string* reason) {
    const std::string lock_name = path(".memftprt_" + std::to_string(target_pid) + ".lock");
    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    lock_fd_ = open(lock_name.c_str(), flags, 0600);
    if (lock_fd_ < 0) {
        *reason = "could not create output lock " + lock_name + ": " + std::strerror(errno);
        return false;
    }

    struct stat lock_status {};
    int lock_error = 0;
    if (fstat(lock_fd_, &lock_status) < 0) {
        lock_error = errno;
    } else if (!S_ISREG(lock_status.st_mode)) {
        lock_error = EINVAL;
    } else if (fchmod(lock_fd_, 0600) < 0) {
        lock_error = errno;
    }
    if (lock_error != 0) {
        release_lock();
        *reason = "output lock " + lock_name + " is not a secure regular file: " +
                  std::strerror(lock_error);
        return false;
    }
    if (flock(lock_fd_, LOCK_EX | LOCK_NB) < 0) {
        const int saved_errno = errno;
        release_lock();
        *reason = "another collector is already writing artifacts for process " +
                  std::to_string(target_pid) + ": " + std::strerror(saved_errno);
        return false;
    }
    return true;
}

void ArtifactStore::release_lock() {
    if (lock_fd_ >= 0) {
        close(lock_fd_);
        lock_fd_ = -1;
    }
}

std::string ArtifactStore::path(const std::string& filename) const {
    return (directory_ / filename).string();
}

const std::filesystem::path& ArtifactStore::directory() const {
    return directory_;
}

std::size_t ArtifactStore::committed_count() const {
    return committed_artifacts_.size();
}

const std::vector<std::string>& ArtifactStore::committed_artifacts() const {
    return committed_artifacts_;
}

void ArtifactStore::discard_temp(TempFile* temporary) {
    if (temporary->fd >= 0) {
        close(temporary->fd);
        temporary->fd = -1;
    }
    if (!temporary->path.empty()) {
        unlink(temporary->path.c_str());
        temporary->path.clear();
    }
}

bool ArtifactStore::create_temp(const std::string& output_name, TempFile* temporary,
                                std::string* reason) {
    std::string pattern = output_name + ".tmp.XXXXXX";
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    const int fd = mkstemp(writable_pattern.data());
    if (fd < 0) {
        *reason = "could not securely create a temporary file for " + output_name + ": " +
                  std::strerror(errno);
        return false;
    }
    temporary->fd = fd;
    temporary->path = writable_pattern.data();

    struct stat status {};
    int secure_error = 0;
    if (fstat(fd, &status) < 0) {
        secure_error = errno;
    } else if (!S_ISREG(status.st_mode)) {
        secure_error = EINVAL;
    } else if (fchmod(fd, 0600) < 0) {
        secure_error = errno;
    } else if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        secure_error = errno;
    }
    if (secure_error != 0) {
        discard_temp(temporary);
        *reason = "could not secure temporary output for " + output_name + ": " +
                  std::strerror(secure_error);
        return false;
    }
    return true;
}

bool ArtifactStore::write_all(int fd, const std::string& contents, std::string* reason) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        const int write_error = count == 0 ? EIO : errno;
        *reason = "write failed: " + std::string(std::strerror(write_error));
        return false;
    }
    return true;
}

bool ArtifactStore::commit_temp(TempFile* temporary, const std::string& output_name,
                                std::string* reason) {
    if (fsync(temporary->fd) < 0) {
        *reason = "fsync failed for " + temporary->path + ": " + std::strerror(errno);
        discard_temp(temporary);
        return false;
    }
    if (close(temporary->fd) < 0) {
        temporary->fd = -1;
        *reason = "close failed for " + temporary->path + ": " + std::strerror(errno);
        discard_temp(temporary);
        return false;
    }
    temporary->fd = -1;
    if (rename(temporary->path.c_str(), output_name.c_str()) < 0) {
        *reason = "rename " + temporary->path + " to " + output_name + " failed: " +
                  std::strerror(errno);
        discard_temp(temporary);
        return false;
    }
    temporary->path.clear();
    if (!sync_directory(reason)) {
        const std::string sync_failure = *reason;
        if (unlink(output_name.c_str()) < 0) {
            *reason = sync_failure + "; could not remove unsynced artifact " + output_name +
                      ": " + std::strerror(errno);
        } else {
            std::string cleanup_reason;
            if (!sync_directory(&cleanup_reason)) {
                *reason = sync_failure + "; cleanup was also not durable: " + cleanup_reason;
            }
        }
        return false;
    }
    committed_artifacts_.push_back(output_name);
    return true;
}

bool ArtifactStore::write_text(const std::string& output_name, const std::string& contents,
                               std::string* reason) {
    TempFile temporary;
    if (!create_temp(output_name, &temporary, reason)) return false;
    if (!write_all(temporary.fd, contents, reason)) {
        discard_temp(&temporary);
        return false;
    }
    return commit_temp(&temporary, output_name, reason);
}

bool ArtifactStore::remove_file(const std::string& output_name, bool missing_ok,
                                std::string* reason) {
    if (unlink(output_name.c_str()) < 0) {
        if (missing_ok && errno == ENOENT) return true;
        *reason = "could not remove " + output_name + ": " + std::strerror(errno);
        return false;
    }
    return sync_directory(reason);
}

bool ArtifactStore::sync_directory(std::string* reason) {
    if (directory_fd_ < 0) {
        *reason = "output directory is not prepared";
        return false;
    }
    if (fsync(directory_fd_) < 0) {
        *reason = "could not fsync output directory " + directory_.string() + ": " +
                  std::strerror(errno);
        return false;
    }
    return true;
}

bool ArtifactStore::sync_created_directory_chain(std::size_t created_directory_count,
                                                 std::string* reason) {
    // Persist both each newly created directory inode and the parent entry that
    // names it. For N created components this means syncing the final directory
    // and then N ancestors, deepest first.
    std::filesystem::path current = directory_;
    for (std::size_t depth = 0; depth <= created_directory_count; ++depth) {
        int fd = -1;
        if (depth == 0) {
            fd = dup(directory_fd_);
        } else {
            fd = open(current.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }
        if (fd < 0) {
            *reason = "could not open output-directory durability path " + current.string() +
                      ": " + std::strerror(errno);
            return false;
        }
        if (fsync(fd) < 0) {
            const int saved_errno = errno;
            close(fd);
            *reason = "could not fsync output-directory durability path " + current.string() +
                      ": " + std::strerror(saved_errno);
            return false;
        }
        if (close(fd) < 0) {
            *reason = "could not close output-directory durability path " + current.string() +
                      ": " + std::strerror(errno);
            return false;
        }
        current = current.parent_path();
    }
    return true;
}

}  // namespace memftprt
