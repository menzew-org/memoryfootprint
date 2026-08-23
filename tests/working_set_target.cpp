/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deterministic target for referenced-working-set integration tests.
 */

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "stable";
    const long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) return 1;
    const std::size_t page_size = static_cast<std::size_t>(page_size_value);
    constexpr std::size_t page_count = 1024;
    const std::size_t length = page_size * page_count;
    void* allocation =
        mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocation == MAP_FAILED) return 2;
    (void)prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, allocation, length,
                "memftprt-working-set");

    auto* bytes = static_cast<volatile unsigned char*>(allocation);
    for (std::size_t page = 0; page < page_count; ++page) {
        bytes[page * page_size] = 1;
    }

    if (mode == "many-maps") {
        constexpr std::size_t mapping_count = 50000;
        const std::uintptr_t base = sizeof(void*) >= 8 ? 0x400000000000ULL : 0x40000000ULL;
        std::size_t mapped = 0;
        for (std::size_t index = 0; index < mapping_count; ++index) {
            void* address = reinterpret_cast<void*>(
                base + index * static_cast<std::uintptr_t>(page_size) * 2U);
            if (mmap(address, page_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED) {
                ++mapped;
            }
        }
        if (mapped < 40000) return 9;
        std::fprintf(stderr, "many-maps-ready=%zu\n", mapped);
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(30);
    bool churn_created = false;
    void* churn_mapping = MAP_FAILED;
    while (std::chrono::steady_clock::now() < deadline) {
        // Repeatedly reference a bounded part of an otherwise resident VMA.
        for (std::size_t page = 0; page < 64; ++page) {
            bytes[page * page_size] =
                static_cast<unsigned char>(bytes[page * page_size] + 1);
        }
        if (!churn_created &&
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(1500)) {
            churn_created = true;
            if (mode == "mapping-churn") {
                churn_mapping = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (churn_mapping == MAP_FAILED) return 4;
                *static_cast<volatile unsigned char*>(churn_mapping) = 1;
            } else if (mode == "process-churn") {
                const pid_t child = fork();
                if (child < 0) return 5;
                if (child == 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                    _exit(0);
                }
            } else if (mode == "transient-process-churn") {
                const pid_t child = fork();
                if (child < 0) return 6;
                if (child == 0) _exit(0);
                int status = 0;
                if (waitpid(child, &status, 0) != child) return 7;
            } else if (mode == "external-stop") {
                if (raise(SIGSTOP) != 0) return 8;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (churn_mapping != MAP_FAILED) (void)munmap(churn_mapping, page_size);
    return munmap(allocation, length) == 0 ? 0 : 3;
}
