#include <chrono>
#include <csignal>
#include <cstddef>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

volatile sig_atomic_t trap_count = 0;

void trap_handler(int) {
    ++trap_count;
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "exec") {
        execl("/bin/sleep", "sleep", "5", static_cast<char*>(nullptr));
        return 3;
    }
    if (mode == "zombie") {
        const pid_t child = fork();
        if (child < 0) return 4;
        if (child == 0) _exit(0);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 0;
    }
    if (mode == "orphan") {
        if (argc != 3) return 5;
        const pid_t child = fork();
        if (child < 0) return 6;
        if (child == 0) {
            const pid_t grandchild = fork();
            if (grandchild < 0) _exit(7);
            if (grandchild > 0) _exit(0);
            std::vector<unsigned char> allocation(12U * 1024U * 1024U, 1U);
            std::ofstream marker(argv[2]);
            marker << getpid() << '\n';
            marker.close();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            _exit(allocation[0] == 1U && marker ? 0 : 8);
        }
        int status = 0;
        if (waitpid(child, &status, 0) < 0) return 9;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 0;
    }
    if (mode == "daemon-exit") {
        if (argc != 3) return 13;
        const pid_t child = fork();
        if (child < 0) return 14;
        if (child == 0) {
            std::vector<unsigned char> allocation(12U * 1024U * 1024U, 1U);
            std::ofstream marker(argv[2]);
            marker << getpid() << '\n';
            marker.close();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            _exit(allocation[0] == 1U && marker ? 0 : 15);
        }
        return 0;
    }
    if (mode == "signal-storm") {
        if (argc != 3) return 10;
        std::signal(SIGTRAP, trap_handler);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            raise(SIGTRAP);
        }
        std::ofstream marker(argv[2]);
        marker << trap_count << '\n';
        return trap_count > 0 && marker ? 0 : 11;
    }
    return 12;
}
