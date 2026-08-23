/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Secure artifact publication API: locked output ownership, randomized
 * temporary files, file and directory fsync, and atomic rename into final
 * paths.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace memftprt {

class ArtifactStore {
public:
    struct TempFile {
        int fd = -1;
        std::string path;
    };

    explicit ArtifactStore(std::filesystem::path directory);
    ~ArtifactStore();

    ArtifactStore(const ArtifactStore&) = delete;
    ArtifactStore& operator=(const ArtifactStore&) = delete;

    bool prepare(std::string* reason);
    bool acquire_lock(std::int64_t target_pid, std::string* reason);
    void release_lock();

    std::string path(const std::string& filename) const;
    const std::filesystem::path& directory() const;
    std::size_t committed_count() const;
    const std::vector<std::string>& committed_artifacts() const;

    bool create_temp(const std::string& output_name, TempFile* temporary,
                     std::string* reason);
    void discard_temp(TempFile* temporary);
    bool commit_temp(TempFile* temporary, const std::string& output_name,
                     std::string* reason);
    bool write_text(const std::string& output_name, const std::string& contents,
                    std::string* reason);
    bool remove_file(const std::string& output_name, bool missing_ok,
                     std::string* reason);

private:
    bool write_all(int fd, const std::string& contents, std::string* reason);
    bool sync_directory(std::string* reason);
    bool sync_created_directory_chain(std::size_t created_directory_count,
                                      std::string* reason);

    std::filesystem::path directory_;
    std::vector<std::string> committed_artifacts_;
    int lock_fd_ = -1;
    int directory_fd_ = -1;
};

}  // namespace memftprt
