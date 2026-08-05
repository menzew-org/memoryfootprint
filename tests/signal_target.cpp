#include <csignal>
#include <fstream>
#include <string>
#include <thread>

#include <chrono>
#include <unistd.h>

volatile sig_atomic_t trap_seen = 0;

void trap_handler(int) {
    trap_seen = 1;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    const std::string mode = argv[1];
    const std::string marker = argv[2];
    if (mode == "trap") {
        std::signal(SIGTRAP, trap_handler);
        raise(SIGTRAP);
        if (trap_seen == 0) {
            return 3;
        }
    } else if (mode == "stop") {
        raise(SIGSTOP);
    } else {
        return 4;
    }

    std::ofstream output(marker);
    output << getpid() << '\n';
    output.close();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return output ? 0 : 5;
}
