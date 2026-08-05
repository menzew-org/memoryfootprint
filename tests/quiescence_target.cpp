#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int run_workload(std::size_t bytes, std::chrono::milliseconds busy_time) {
    std::vector<unsigned char> memory(bytes, 0);
    volatile std::uint64_t checksum = 0;
    const auto busy_until = std::chrono::steady_clock::now() + busy_time;
    while (std::chrono::steady_clock::now() < busy_until) {
        for (std::size_t offset = 0; offset < memory.size(); offset += 4096) {
            memory[offset] = static_cast<unsigned char>(memory[offset] + 1U);
            checksum += memory[offset];
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return checksum == 0 ? 1 : 0;
}

int main() {
    // The child remains busy longer than the parent. A root-only detector would
    // declare quiescence too early; a tree-aware detector must observe both.
    const pid_t child = fork();
    if (child < 0) {
        return 2;
    }
    if (child == 0) {
        return run_workload(8U * 1024U * 1024U, std::chrono::milliseconds(1200));
    }

    const int parent_result =
        run_workload(16U * 1024U * 1024U, std::chrono::milliseconds(800));
    int child_status = 0;
    if (waitpid(child, &child_status, 0) < 0 || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 3;
    }
    return parent_result;
}
