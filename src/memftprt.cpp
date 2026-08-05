/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Buildable reconstruction of memftprt 1.39.
 *
 * This is maintained reconstructed source, not original source text.
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using ulong_t = unsigned long;

struct fp_metric {
    const char* name;
    ulong_t value;
    const char* units;
    fp_metric* next;
};

struct fp_prochandle {
    pid_t pid;
    unsigned short flags;
    int status;
    int cursig;
};

struct proc_stats {
    unsigned long stat_minflt;
    unsigned long stat_majflt;
    unsigned long stat_vsize;
    long stat_rss;
    unsigned long stat_rlim;
    unsigned long stat_nswap;

    long statm_size;
    long statm_resident;
    long statm_share;
    long statm_trs;
    long statm_lrs;
    long statm_drs;
    long statm_dt;

    long status_vmpeak;
    long status_vmsize;
    long status_vmlock;
    long status_vmhwm;
    long status_vmrss;
    long status_vmdata;
    long status_vmstack;
    long status_vmexe;
    long status_vmlib;
    long status_vmpte;
};

static_assert(sizeof(fp_metric) == 32, "DWARF says fp_metric is 32 bytes");
static_assert(sizeof(fp_prochandle) == 16, "DWARF says fp_prochandle is 16 bytes");
static_assert(sizeof(proc_stats) == 184, "DWARF says proc_stats is 184 bytes");

namespace {

constexpr unsigned short FP_CREATED = 0x01;
constexpr unsigned short FP_GRABBED = 0x02;
constexpr unsigned short FP_STOPPING = 0x04;
constexpr unsigned short FP_STOPPED = 0x40;
constexpr unsigned short FP_LIVE = FP_CREATED | FP_GRABBED;
constexpr const char* MEMFTPRT_VERSION = "1.39-reconstructed";

int debug_level = 0;
const char* program_name = "memftprt";
fp_prochandle* target_process = nullptr;
fp_metric* fp_metrics = nullptr;
bool redirect_debug = false;
int wait_time = -1;
unsigned quiet_window_seconds = 3;
unsigned quiescence_timeout_seconds = 300;
bool kill_before_exit = false;
volatile sig_atomic_t exiting = 0;
struct stopped_process {
    pid_t pid;
    unsigned long start_time;
    int pidfd;
};

std::map<pid_t, stopped_process> descendants_stopped_by_us;
std::string frozen_cgroup_path;
int expected_initial_stop_signal = 0;
bool root_stopped_by_us = false;
bool root_externally_stopped = false;
bool target_launched = false;
pid_t original_launcher_pid = -1;
bool quiet_mode = false;
std::filesystem::path output_directory = ".";
std::vector<std::string> committed_artifacts;

enum class freeze_mode {
    automatic,
    cgroup,
    signals,
};

freeze_mode requested_freeze_mode = freeze_mode::automatic;
bool jcmd_enabled = true;
bool enable_jvm_nmt = false;
std::string selected_freeze_backend = "none";
std::string selected_cgroup_path;
std::string requested_launch_cgroup;
struct runtime_capture_metadata {
    std::string status;
    std::string runtime = "hotspot";
    std::string runtime_version;
    unsigned long long timestamp_ns = 0;
    unsigned long long duration_ns = 0;
    std::string method = "jcmd VM.native_memory summary";
    std::string units = "KB";
    bool forced_gc = false;
};
std::map<pid_t, runtime_capture_metadata> jvm_capture_status;
int output_lock_fd = -1;

void release_process(fp_prochandle* fp_proc);
int kill_process(fp_prochandle* fp_proc);
void continue_running(fp_prochandle* fp_proc);
bool resume_stopped_descendants();
bool thaw_cgroup();
std::set<pid_t> discover_process_tree(pid_t root_pid);
bool retarget_to_adopted_descendant(fp_prochandle* fp_proc);
void reap_adopted_zombies(pid_t protected_pid);
bool read_process_state(pid_t pid, char* state);

void d_printf(int level, const char* format, ...) {
    if (quiet_mode || debug_level < level) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(redirect_debug ? stdout : stderr, format, args);
    va_end(args);
}

void user_message(const char* format, ...) {
    if (quiet_mode) return;
    std::fprintf(stderr, "%s: ", program_name);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

[[maybe_unused]] void d_fprintf(FILE* stream, int level, const char* format, ...) {
    if (quiet_mode || debug_level < level) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(stream, format, args);
    va_end(args);
}

[[noreturn]] void abend(const char* format, ...) {
    std::fprintf(stderr, "%s: aborting - ", program_name);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);

    (void)thaw_cgroup();
    (void)resume_stopped_descendants();
    if (target_process != nullptr) {
        if (kill_before_exit) {
            kill_process(target_process);
        } else {
            release_process(target_process);
        }
        target_process = nullptr;
    }
    std::exit(254);
}

void fp_assert(bool expression, const char* format, ...) {
    if (expression) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    (void)thaw_cgroup();
    (void)resume_stopped_descendants();
    if (target_process != nullptr) {
        release_process(target_process);
        target_process = nullptr;
    }
    std::exit(255);
}

void add_metric(const char* name, ulong_t value, const char* units) {
    auto* metric = static_cast<fp_metric*>(std::malloc(sizeof(fp_metric)));
    if (metric == nullptr) {
        abend("add_metric(): could not allocate memory: %s\n", std::strerror(errno));
    }
    metric->name = name;
    metric->value = value;
    metric->units = units;
    metric->next = fp_metrics;
    fp_metrics = metric;
}

void report() {
    for (fp_metric* metric = fp_metrics; metric != nullptr; metric = metric->next) {
        std::fprintf(stderr, "%-31s  %10lu  %-20s\n", metric->name, metric->value,
                     metric->units);
    }
}

void announce_wait(pid_t pid, const char* action) {
    if (wait_time == -1) {
        user_message("%s process %d; waiting for %us of quiescence (timeout %us)\n", action,
                     pid, quiet_window_seconds, quiescence_timeout_seconds);
    } else {
        user_message("%s process %d; waiting %d seconds before capture\n", action, pid,
                     wait_time);
    }
}

void announce_completion(pid_t pid) {
    user_message("capture complete for process %d using %s freeze; %zu artifacts in %s\n", pid,
                 selected_freeze_backend.c_str(), committed_artifacts.size(),
                 output_directory.c_str());
}

std::string output_path(const std::string& filename) {
    return (output_directory / filename).string();
}

void acquire_output_lock(pid_t root_pid) {
    const std::string lock_name =
        output_path(".memftprt_" + std::to_string(root_pid) + ".lock");
    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    output_lock_fd = open(lock_name.c_str(), flags, 0600);
    if (output_lock_fd < 0) {
        abend("could not create output lock %s: %s\n", lock_name.c_str(), std::strerror(errno));
    }
    struct stat lock_status {};
    int lock_error = 0;
    if (fstat(output_lock_fd, &lock_status) < 0) {
        lock_error = errno;
    } else if (!S_ISREG(lock_status.st_mode)) {
        lock_error = EINVAL;
    } else if (fchmod(output_lock_fd, 0600) < 0) {
        lock_error = errno;
    }
    if (lock_error != 0) {
        close(output_lock_fd);
        output_lock_fd = -1;
        abend("output lock %s is not a secure regular file: %s\n", lock_name.c_str(),
              std::strerror(lock_error));
    }
    if (flock(output_lock_fd, LOCK_EX | LOCK_NB) < 0) {
        const int saved_errno = errno;
        close(output_lock_fd);
        output_lock_fd = -1;
        abend("another collector is already writing artifacts for process %d: %s\n", root_pid,
              std::strerror(saved_errno));
    }
}

void release_output_lock() {
    if (output_lock_fd >= 0) {
        close(output_lock_fd);
        output_lock_fd = -1;
    }
}

struct secure_temp_file {
    int fd = -1;
    std::string path;
};

void discard_secure_temp(secure_temp_file* temporary) {
    if (temporary->fd >= 0) {
        close(temporary->fd);
        temporary->fd = -1;
    }
    if (!temporary->path.empty()) {
        unlink(temporary->path.c_str());
        temporary->path.clear();
    }
}

bool create_secure_temp(const std::string& output_name, secure_temp_file* temporary,
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
        discard_secure_temp(temporary);
        *reason = "could not secure temporary output for " + output_name + ": " +
                  std::strerror(secure_error);
        return false;
    }
    return true;
}

bool write_all(int fd, const std::string& contents, std::string* reason) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        const int write_error = count == 0 ? EIO : errno;
        *reason = "write failed: " + std::string(std::strerror(write_error));
        return false;
    }
    return true;
}

bool commit_secure_temp(secure_temp_file* temporary, const std::string& output_name,
                        std::string* reason) {
    if (fsync(temporary->fd) < 0) {
        *reason = "fsync failed for " + temporary->path + ": " + std::strerror(errno);
        discard_secure_temp(temporary);
        return false;
    }
    if (close(temporary->fd) < 0) {
        temporary->fd = -1;
        *reason = "close failed for " + temporary->path + ": " + std::strerror(errno);
        discard_secure_temp(temporary);
        return false;
    }
    temporary->fd = -1;
    if (rename(temporary->path.c_str(), output_name.c_str()) < 0) {
        *reason = "rename " + temporary->path + " to " + output_name + " failed: " +
                  std::strerror(errno);
        discard_secure_temp(temporary);
        return false;
    }
    temporary->path.clear();
    committed_artifacts.push_back(output_name);
    return true;
}

bool write_atomic_text(const std::string& output_name, const std::string& contents,
                       std::string* reason) {
    secure_temp_file temporary;
    if (!create_secure_temp(output_name, &temporary, reason)) {
        return false;
    }
    if (!write_all(temporary.fd, contents, reason)) {
        discard_secure_temp(&temporary);
        return false;
    }
    return commit_secure_temp(&temporary, output_name, reason);
}

void print_help(FILE* stream) {
    std::fprintf(stream, "memftprt %s - consistent process-tree memory capture\n\n",
                 MEMFTPRT_VERSION);
    std::fprintf(stream, "Usage:\n");
    std::fprintf(stream, "  %s [options] -- command [args...]\n", program_name);
    std::fprintf(stream, "  %s [options] --pid PID\n\n", program_name);
    std::fprintf(stream, "Capture timing:\n");
    std::fprintf(stream, "  -q, --stable-window SEC  required stable window (default: 3)\n");
    std::fprintf(stream, "  -m, --timeout SEC        maximum quiescence wait (default: 300)\n");
    std::fprintf(stream, "  -t, --delay SEC          use a fixed delay instead of quiescence\n\n");
    std::fprintf(stream, "Target and consistency:\n");
    std::fprintf(stream, "  -p, --pid PID            attach to an existing process\n");
    std::fprintf(stream, "      --freeze MODE        auto, cgroup, or signals (default: auto)\n");
    std::fprintf(stream, "      --cgroup PATH        launch in an empty delegated cgroup v2\n\n");
    std::fprintf(stream, "Runtime capture:\n");
    std::fprintf(stream, "      --enable-jvm-nmt     enable HotSpot NMT for a launched JVM\n");
    std::fprintf(stream, "      --no-jcmd            disable automatic JVM NMT collection\n\n");
    std::fprintf(stream, "Output and diagnostics:\n");
    std::fprintf(stream, "  -o, --output-dir DIR     write artifacts to DIR (default: current directory)\n");
    std::fprintf(stream, "      --quiet              emit nothing on successful capture\n");
    std::fprintf(stream, "  -v, --verbose            show capture decisions; repeat for more detail\n");
    std::fprintf(stream, "  -h, --help               show this help and exit\n");
    std::fprintf(stream, "  -V, --version            show version and exit\n\n");
    std::fprintf(stream, "Examples:\n");
    std::fprintf(stream, "  %s -o captures -- ./server --port 8080\n", program_name);
    std::fprintf(stream, "  %s --pid 1234 --stable-window 5\n", program_name);
    std::fprintf(stream, "  %s --enable-jvm-nmt -- java -jar app.jar\n", program_name);
}

[[noreturn]] void cli_error(const char* format, ...) {
    std::fprintf(stderr, "%s: error: ", program_name);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fprintf(stderr, "\nTry '%s --help' for usage.\n", program_name);
    std::exit(2);
}

void prepare_output_directory() {
    std::error_code error;
    if (!std::filesystem::exists(output_directory, error)) {
        (void)std::filesystem::create_directories(output_directory, error);
        if (error) {
            cli_error("could not create output directory %s: %s",
                      output_directory.c_str(), error.message().c_str());
        }
    }
    if (error || !std::filesystem::is_directory(output_directory, error)) {
        cli_error("output path is not a directory: %s", output_directory.c_str());
    }
    output_directory = std::filesystem::canonical(output_directory, error);
    if (error || access(output_directory.c_str(), W_OK | X_OK) < 0) {
        cli_error("output directory is not writable: %s", output_directory.c_str());
    }
}

void termination_handler(int signal_number) {
    exiting = signal_number;
}

void initialize_library() {
    struct sigaction action {};
    action.sa_handler = termination_handler;
    action.sa_flags = SA_RESTART;
    sigfillset(&action.sa_mask);
    constexpr int handled_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (int signal_number : handled_signals) {
        if (sigaction(signal_number, &action, nullptr) < 0) {
            abend("sigaction(%d) failed: %s\n", signal_number, std::strerror(errno));
        }
    }
}

bool consume_initial_trace_stop(fp_prochandle* fp_proc) {
    if (expected_initial_stop_signal == 0 || fp_proc->cursig != expected_initial_stop_signal) {
        return false;
    }
    expected_initial_stop_signal = 0;
    fp_proc->cursig = 0;
    if (ptrace(PTRACE_SETOPTIONS, fp_proc->pid, nullptr,
               static_cast<unsigned long>(PTRACE_O_TRACEEXEC)) < 0) {
        abend("ptrace(PTRACE_SETOPTIONS, %d, PTRACE_O_TRACEEXEC) failed: %s\n",
              fp_proc->pid, std::strerror(errno));
    }
    return true;
}

bool consume_ptrace_event(fp_prochandle* fp_proc) {
    const unsigned event = static_cast<unsigned>(fp_proc->status) >> 16U;
    if (fp_proc->cursig != SIGTRAP || event == 0) {
        return false;
    }
    fp_proc->cursig = 0;
    return true;
}

bool is_group_stop_signal(int signal_number) {
    return signal_number == SIGSTOP || signal_number == SIGTSTP ||
           signal_number == SIGTTIN || signal_number == SIGTTOU;
}

bool is_ptrace_group_stop(fp_prochandle* fp_proc) {
    if (!is_group_stop_signal(fp_proc->cursig)) {
        return false;
    }
    siginfo_t signal_info {};
    errno = 0;
    return ptrace(PTRACE_GETSIGINFO, fp_proc->pid, nullptr, &signal_info) < 0 && errno == EINVAL;
}

bool resume_after_target_stop(fp_prochandle* fp_proc) {
    if (consume_initial_trace_stop(fp_proc)) {
        if (root_externally_stopped) {
            d_printf(1, "target was already stopped before attach; capturing without resuming it\n");
            return false;
        }
        continue_running(fp_proc);
        return true;
    }
    if (consume_ptrace_event(fp_proc)) {
        continue_running(fp_proc);
        return true;
    }
    if (is_ptrace_group_stop(fp_proc)) {
        root_externally_stopped = true;
        fp_proc->cursig = 0;
        d_printf(1, "target entered an external job-control stop; capturing without resuming it\n");
        return false;
    }
    // Preserve genuine signals, including SIGTRAP and the signal-delivery stop
    // that precedes a job-control group stop.
    continue_running(fp_proc);
    return true;
}

void continue_running(fp_prochandle* fp_proc) {
    if (fp_proc == nullptr) {
        abend("continue_running: fp_proc is NULL\n");
    }
    if ((fp_proc->flags & FP_LIVE) == 0) {
        abend("continue_running: target has died\n");
    }
    fp_assert(fp_proc->pid != getpid(), "invoked on self");

    d_printf(3, "continue_running: resuming process\n");
    fp_proc->flags &= static_cast<unsigned short>(~(FP_STOPPING | FP_STOPPED));
    const int signal_to_forward = fp_proc->cursig > 0 ? fp_proc->cursig : 0;
    if (ptrace(PTRACE_CONT, fp_proc->pid, nullptr, signal_to_forward) < 0) {
        abend("ptrace(PTRACE_CONT, %d, NULL, %d) failed: %s\n", fp_proc->pid,
              signal_to_forward, std::strerror(errno));
    }
    d_printf(3, "continue_running: process resumed\n");
}

int wait_nohang(fp_prochandle* fp_proc) {
    fp_proc->status = 0;
    d_printf(4, "wait_nohang: inspect status of target\n");
    const pid_t result = waitpid(fp_proc->pid, &fp_proc->status, WNOHANG | WUNTRACED);
    if (result < 0) {
        if (errno == ECHILD) {
            d_printf(4, "wait_nohang: ECHILD: target has detached\n");
        } else {
            d_printf(4, "wait_nohang: error from waitpid(): %s\n", std::strerror(errno));
        }
        return result;
    }
    if (result == 0) {
        fp_proc->flags &= static_cast<unsigned short>(~FP_STOPPED);
        d_printf(4, "wait_nohang: target is running\n");
    } else if (WIFSIGNALED(fp_proc->status)) {
        fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
        d_printf(4, "wait_nohang: target was terminated by a signal\n");
    } else if (WIFEXITED(fp_proc->status)) {
        fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
        d_printf(4, "wait_nohang: target has exited with status %d\n",
                 WEXITSTATUS(fp_proc->status));
    } else if (WIFSTOPPED(fp_proc->status)) {
        fp_proc->cursig = WSTOPSIG(fp_proc->status);
        fp_proc->flags = static_cast<unsigned short>((fp_proc->flags & ~FP_STOPPING) | FP_STOPPED);
        d_printf(4, "wait_nohang: target has stopped on signal %d\n", fp_proc->cursig);
    }
    return result;
}

bool stop_and_detach(fp_prochandle* fp_proc) {
    if (fp_proc == nullptr || (fp_proc->flags & FP_LIVE) == 0) {
        return false;
    }
    fp_proc->flags |= FP_STOPPING;
    root_stopped_by_us = true;
    if (::kill(fp_proc->pid, SIGSTOP) < 0) {
        fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
        return false;
    }

    for (;;) {
        const pid_t result = waitpid(fp_proc->pid, &fp_proc->status, 0);
        if (result < 0) {
            fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
            return false;
        }
        if (WIFEXITED(fp_proc->status) || WIFSIGNALED(fp_proc->status)) {
            fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
            return false;
        }
        if (!WIFSTOPPED(fp_proc->status)) {
            continue;
        }
        fp_proc->cursig = WSTOPSIG(fp_proc->status);
        if (fp_proc->cursig == SIGSTOP) {
            fp_proc->flags &= static_cast<unsigned short>(~FP_STOPPING);
            return ptrace(PTRACE_DETACH, fp_proc->pid, nullptr, 0) == 0;
        }
        if (consume_ptrace_event(fp_proc)) {
            d_printf(3, "stop_and_detach: consumed ptrace event while waiting for SIGSTOP\n");
        }
        continue_running(fp_proc);
        fp_proc->flags |= FP_STOPPING;
    }
}

void release_process(fp_prochandle* fp_proc) {
    if (fp_proc == nullptr) {
        return;
    }
    if ((fp_proc->flags & FP_LIVE) != 0) {
        fp_assert(fp_proc->pid != getpid(), "invoked on self");
        d_printf(3, "release_process: detaching from %d\n", fp_proc->pid);
        if (ptrace(PTRACE_DETACH, fp_proc->pid, nullptr, 0) < 0 && !stop_and_detach(fp_proc)) {
            const int saved_errno = errno;
            const pid_t saved_pid = fp_proc->pid;
            if (target_process == fp_proc) {
                target_process = nullptr;
            }
            delete fp_proc;
            abend("ptrace(PTRACE_DETACH, %d) failed: %s\n", saved_pid,
                  std::strerror(saved_errno));
        }
        if (root_stopped_by_us && (fp_proc->flags & (FP_STOPPING | FP_STOPPED)) != 0) {
            (void)::kill(fp_proc->pid, SIGCONT);
        }
        root_stopped_by_us = false;
        fp_proc->flags &= static_cast<unsigned short>(~(FP_LIVE | FP_STOPPING | FP_STOPPED));
    }
    delete fp_proc;
}

int kill_process(fp_prochandle* fp_proc) {
    if (fp_proc == nullptr) {
        errno = EINVAL;
        return -1;
    }
    if ((fp_proc->flags & FP_LIVE) == 0) {
        delete fp_proc;
        return 0;
    }
    fp_assert(fp_proc->pid != getpid(), "attempting to kill ourself\n");

    if (ptrace(PTRACE_DETACH, fp_proc->pid, nullptr, 0) < 0 && !stop_and_detach(fp_proc)) {
        const int saved_errno = errno;
        const pid_t saved_pid = fp_proc->pid;
        if (target_process == fp_proc) {
            target_process = nullptr;
        }
        delete fp_proc;
        abend("ptrace(PTRACE_DETACH, %d) failed: %s\n", saved_pid,
              std::strerror(saved_errno));
    }
    if (root_stopped_by_us && (fp_proc->flags & (FP_STOPPING | FP_STOPPED)) != 0) {
        (void)::kill(fp_proc->pid, SIGCONT);
    }
    root_stopped_by_us = false;
    if (::kill(fp_proc->pid, SIGTERM) < 0 && errno != ESRCH) {
        delete fp_proc;
        return -1;
    }
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (wait_nohang(fp_proc) < 0 && errno != ECHILD) {
            delete fp_proc;
            return -1;
        }
        if ((fp_proc->flags & FP_LIVE) == 0) {
            delete fp_proc;
            return 0;
        }
    }
    std::fprintf(stderr, "kill_process: process did not terminate - sending SIGKILL\n");
    (void)::kill(fp_proc->pid, SIGKILL);
    delete fp_proc;
    return 0;
}

void continue_until_elapsed(fp_prochandle* fp_proc, unsigned seconds) {
    fp_assert(fp_proc != nullptr, "fp_proc is NULL");
    fp_assert(fp_proc->pid != getpid(), "invoked on self");
    d_printf(3, "continue_until_elapsed: calling wait_on_target:  wait time = %u\n", seconds);

    const auto start = std::chrono::steady_clock::now();
    bool stop_requested = false;
    for (;;) {
        reap_adopted_zombies(fp_proc->pid);
        fp_proc->status = 0;
        const pid_t result = waitpid(fp_proc->pid, &fp_proc->status, WNOHANG | WUNTRACED);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            abend("wait_impl: unexpected error from waitpid(): %s\n", std::strerror(errno));
        }

        const bool elapsed = std::chrono::steady_clock::now() - start >=
                             std::chrono::seconds(seconds);
        if (result == fp_proc->pid) {
            if (WIFSIGNALED(fp_proc->status)) {
                fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
                if (retarget_to_adopted_descendant(fp_proc)) {
                    stop_requested = false;
                    continue;
                }
                abend("target exited before measurement could be taken: termination signal = %d\n",
                      WTERMSIG(fp_proc->status));
            }
            if (WIFEXITED(fp_proc->status)) {
                fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
                if (retarget_to_adopted_descendant(fp_proc)) {
                    stop_requested = false;
                    continue;
                }
                abend("target exited before measurement could be taken: exitcode = %d\n",
                      WEXITSTATUS(fp_proc->status));
            }
            if (WIFSTOPPED(fp_proc->status)) {
                fp_proc->cursig = WSTOPSIG(fp_proc->status);
                fp_proc->flags |= FP_STOPPED;
                d_printf(4, "wait_on_target: target STOPPED with signal: %d\n", fp_proc->cursig);
                if (stop_requested && fp_proc->cursig == SIGSTOP) {
                    fp_proc->flags &= static_cast<unsigned short>(~FP_STOPPING);
                    fp_proc->cursig = 0;
                    d_printf(4, "wait_on_target: the wait is over!\n");
                    return;
                }
                if (!resume_after_target_stop(fp_proc)) {
                    return;
                }
            }
        }

        if (!stop_requested && (elapsed || exiting != 0)) {
            d_printf(4, "wait_on_target: timer elapsed... stopping target.\n");
            fp_proc->flags |= FP_STOPPING;
            root_stopped_by_us = true;
            if (::kill(fp_proc->pid, SIGSTOP) < 0) {
                abend("could not stop target: kill(%d, SIGSTOP) failed: %s\n", fp_proc->pid,
                      std::strerror(errno));
            }
            stop_requested = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

char* read_file(const char* dirname, const char* file) {
    char pathname[1024];
    fp_assert(std::strlen(dirname) + 1 + std::strlen(file) < sizeof(pathname),
              "buffer overflow emminent\n");
    std::snprintf(pathname, sizeof(pathname), "%s/%s", dirname, file);

    const int fd = open(pathname, O_RDONLY);
    if (fd < 0) {
        abend("open %s failed: %s\n", pathname, std::strerror(errno));
    }

    constexpr size_t increment = 1024;
    size_t capacity = 0;
    size_t used = 0;
    char* buffer = nullptr;
    for (;;) {
        if (capacity - used < 2) {
            capacity += increment;
            char* expanded = static_cast<char*>(std::realloc(buffer, capacity));
            if (expanded == nullptr) {
                const int saved_errno = errno;
                std::free(buffer);
                close(fd);
                abend("realloc %s failed: %s\n", pathname, std::strerror(saved_errno));
            }
            buffer = expanded;
        }

        const ssize_t count = read(fd, buffer + used, capacity - used - 1);
        if (count > 0) {
            used += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            const int saved_errno = errno;
            std::free(buffer);
            close(fd);
            abend("read %s failed: %s\n", pathname, std::strerror(saved_errno));
        }
        buffer[used] = '\0';
        close(fd);
        return buffer;
    }
}

bool read_kb_field(const char* status, const char* field, long* value) {
    const char* start = std::strstr(status, field);
    if (start == nullptr) {
        return false;
    }
    start += std::strlen(field);
    return std::sscanf(start, "%ld kB", value) == 1;
}

unsigned long parse_unsigned(const std::string& value) {
    errno = 0;
    char* end = nullptr;
    const unsigned long result = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        abend("could not parse /proc numeric field: %s\n", value.c_str());
    }
    return result;
}

long parse_signed(const std::string& value) {
    errno = 0;
    char* end = nullptr;
    const long result = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        abend("could not parse /proc numeric field: %s\n", value.c_str());
    }
    return result;
}

struct process_sample {
    unsigned long cpu_ticks;
    unsigned long virtual_bytes;
    long resident_pages;
    char state;
    std::size_t process_count;
    std::set<std::pair<pid_t, unsigned long>> process_identities;
};

bool parse_pid_name(const char* name, pid_t* pid) {
    if (name == nullptr || *name == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(name, &end, 10);
    if (errno != 0 || end == name || *end != '\0' || value < 1 ||
        value > std::numeric_limits<pid_t>::max()) {
        return false;
    }
    *pid = static_cast<pid_t>(value);
    return true;
}

bool read_proc_stat_fields(pid_t pid, std::string* command, std::vector<std::string>* fields) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!input || !std::getline(input, line)) {
        return false;
    }
    const std::size_t left_parenthesis = line.find('(');
    const std::size_t right_parenthesis = line.rfind(')');
    if (left_parenthesis == std::string::npos || right_parenthesis == std::string::npos ||
        right_parenthesis <= left_parenthesis || right_parenthesis + 2 > line.size()) {
        return false;
    }
    if (command != nullptr) {
        *command = line.substr(left_parenthesis + 1, right_parenthesis - left_parenthesis - 1);
    }
    std::istringstream fields_stream(line.substr(right_parenthesis + 2));
    fields->clear();
    std::string field;
    while (fields_stream >> field) {
        fields->push_back(field);
    }
    return !fields->empty();
}

std::set<pid_t> discover_process_tree(pid_t root_pid) {
    struct process_parent {
        pid_t pid;
        pid_t parent;
    };
    std::vector<process_parent> processes;
    DIR* proc = opendir("/proc");
    if (proc == nullptr) {
        abend("could not scan /proc: %s\n", std::strerror(errno));
    }
    while (dirent* entry = readdir(proc)) {
        pid_t pid = 0;
        if (!parse_pid_name(entry->d_name, &pid)) {
            continue;
        }
        std::vector<std::string> fields;
        if (!read_proc_stat_fields(pid, nullptr, &fields) || fields.size() < 2) {
            continue;
        }
        char* end = nullptr;
        const long parent = std::strtol(fields[1].c_str(), &end, 10);
        if (end == fields[1].c_str() || *end != '\0' || parent < 0 ||
            parent > std::numeric_limits<pid_t>::max()) {
            continue;
        }
        const char state = fields[0].empty() ? '?' : fields[0][0];
        if (state == 'Z' || state == 'X' || state == 'x') {
            continue;
        }
        processes.push_back(process_parent {pid, static_cast<pid_t>(parent)});
    }
    closedir(proc);

    std::set<pid_t> tree;
    for (const process_parent& process : processes) {
        if (process.pid == root_pid) {
            tree.insert(root_pid);
            break;
        }
    }
    if (target_launched) {
        // PR_SET_CHILD_SUBREAPER reparents daemonized descendants to the
        // collector. Include those adopted children in the launched app tree.
        for (const process_parent& process : processes) {
            if (process.parent == getpid() && process.pid != root_pid) {
                tree.insert(process.pid);
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const process_parent& process : processes) {
            if (tree.count(process.parent) != 0 && tree.insert(process.pid).second) {
                changed = true;
            }
        }
    }
    return tree;
}

void reap_adopted_zombies(pid_t protected_pid) {
    DIR* proc = opendir("/proc");
    if (proc == nullptr) return;
    while (dirent* entry = readdir(proc)) {
        pid_t pid = 0;
        if (!parse_pid_name(entry->d_name, &pid) || pid == protected_pid) continue;
        std::vector<std::string> fields;
        if (!read_proc_stat_fields(pid, nullptr, &fields) || fields.size() < 2) continue;
        if (fields[0].empty() || (fields[0][0] != 'Z' && fields[0][0] != 'X' &&
                                  fields[0][0] != 'x')) {
            continue;
        }
        char* end = nullptr;
        const long parent = std::strtol(fields[1].c_str(), &end, 10);
        if (end != fields[1].c_str() && *end == '\0' && parent == getpid()) {
            int status = 0;
            (void)waitpid(pid, &status, WNOHANG);
        }
    }
    closedir(proc);
}

bool retarget_to_adopted_descendant(fp_prochandle* fp_proc) {
    if (!target_launched || fp_proc == nullptr) return false;
    reap_adopted_zombies(fp_proc->pid);
    const std::set<pid_t> candidates = discover_process_tree(fp_proc->pid);
    for (pid_t candidate : candidates) {
        if (candidate == fp_proc->pid) continue;
        char state = '?';
        const bool was_stopped = read_process_state(candidate, &state) &&
                                 (state == 'T' || state == 't');
        if (ptrace(PTRACE_ATTACH, candidate, nullptr, 0) < 0) {
            if (errno == ESRCH) continue;
            d_printf(1, "could not adopt surviving process %d: %s\n", candidate,
                     std::strerror(errno));
            continue;
        }
        const pid_t previous = fp_proc->pid;
        fp_proc->pid = candidate;
        fp_proc->flags = FP_GRABBED;
        fp_proc->status = 0;
        fp_proc->cursig = 0;
        expected_initial_stop_signal = SIGSTOP;
        root_stopped_by_us = false;
        root_externally_stopped = was_stopped;
        release_output_lock();
        acquire_output_lock(candidate);
        d_printf(1, "launcher %d exited; continuing with adopted application process %d\n",
                 previous, candidate);
        return true;
    }
    return false;
}

bool read_process_start_time(pid_t pid, unsigned long* start_time) {
    std::vector<std::string> fields;
    if (!read_proc_stat_fields(pid, nullptr, &fields) || fields.size() <= 19) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(fields[19].c_str(), &end, 10);
    if (errno != 0 || end == fields[19].c_str() || *end != '\0') {
        return false;
    }
    *start_time = value;
    return true;
}

bool read_process_sample(pid_t pid, process_sample* sample, std::string* command = nullptr) {
    std::vector<std::string> fields;
    // fields[0] is field 3 (state), 11/12 are utime/stime, and 20/21 are
    // virtual size/RSS. Process-level utime and stime include all threads.
    if (!read_proc_stat_fields(pid, command, &fields) || fields.size() <= 21) {
        return false;
    }
    *sample = process_sample {
        parse_unsigned(fields[11]) + parse_unsigned(fields[12]),
        parse_unsigned(fields[20]),
        parse_signed(fields[21]),
        fields[0].empty() ? '?' : fields[0][0],
        1,
        {{pid, parse_unsigned(fields[19])}},
    };
    return true;
}

process_sample read_process_tree_sample(pid_t root_pid) {
    const std::set<pid_t> tree = discover_process_tree(root_pid);
    process_sample total {0, 0, 0, 'S', 0, {}};
    for (pid_t pid : tree) {
        process_sample current {};
        if (!read_process_sample(pid, &current)) {
            if (pid == root_pid) {
                abend("target process %d disappeared while sampling\n", root_pid);
            }
            continue;
        }
        total.cpu_ticks += current.cpu_ticks;
        total.virtual_bytes += current.virtual_bytes;
        total.resident_pages += current.resident_pages;
        ++total.process_count;
        total.process_identities.insert(current.process_identities.begin(),
                                        current.process_identities.end());
        if (current.state == 'R') {
            total.state = 'R';
        } else if (current.state == 'D' && total.state != 'R') {
            total.state = 'D';
        }
    }
    return total;
}

unsigned long absolute_difference(unsigned long left, unsigned long right) {
    return left >= right ? left - right : right - left;
}

unsigned long absolute_difference(long left, long right) {
    return static_cast<unsigned long>(left >= right ? left - right : right - left);
}

void continue_until_quiescent(fp_prochandle* fp_proc) {
    fp_assert(fp_proc != nullptr, "fp_proc is NULL");
    fp_assert(fp_proc->pid != getpid(), "invoked on self");

    constexpr auto sample_interval = std::chrono::milliseconds(500);
    constexpr unsigned long max_cpu_ticks_per_sample = 1;
    constexpr unsigned long max_rss_delta_pages = 1;
    const long page_size_value = sysconf(_SC_PAGESIZE);
    const unsigned long page_size = page_size_value > 0
                                        ? static_cast<unsigned long>(page_size_value)
                                        : 4096UL;

    const auto started = std::chrono::steady_clock::now();
    auto next_sample_at = started;
    auto stable_since = started;
    bool have_previous = false;
    bool stability_window_started = false;
    bool stop_requested = false;
    process_sample previous {};

    d_printf(1,
             "waiting for quiescence: stable for %us, timeout %us, "
             "CPU <= %lu tick/sample, RSS/VM <= 1 page/sample\n",
             quiet_window_seconds, quiescence_timeout_seconds, max_cpu_ticks_per_sample);

    for (;;) {
        reap_adopted_zombies(fp_proc->pid);
        fp_proc->status = 0;
        const pid_t result = waitpid(fp_proc->pid, &fp_proc->status, WNOHANG | WUNTRACED);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            abend("wait_impl: unexpected error from waitpid(): %s\n", std::strerror(errno));
        }
        if (result == fp_proc->pid) {
            if (WIFSIGNALED(fp_proc->status)) {
                fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
                if (retarget_to_adopted_descendant(fp_proc)) {
                    have_previous = false;
                    stability_window_started = false;
                    stop_requested = false;
                    continue;
                }
                abend("target exited before measurement could be taken: termination signal = %d\n",
                      WTERMSIG(fp_proc->status));
            }
            if (WIFEXITED(fp_proc->status)) {
                fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
                if (retarget_to_adopted_descendant(fp_proc)) {
                    have_previous = false;
                    stability_window_started = false;
                    stop_requested = false;
                    continue;
                }
                abend("target exited before measurement could be taken: exitcode = %d\n",
                      WEXITSTATUS(fp_proc->status));
            }
            if (WIFSTOPPED(fp_proc->status)) {
                fp_proc->cursig = WSTOPSIG(fp_proc->status);
                fp_proc->flags |= FP_STOPPED;
                if (stop_requested && fp_proc->cursig == SIGSTOP) {
                    fp_proc->flags &= static_cast<unsigned short>(~FP_STOPPING);
                    fp_proc->cursig = 0;
                    d_printf(1, "target stopped; capturing footprint\n");
                    return;
                }
                if (!resume_after_target_stop(fp_proc)) {
                    return;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (!stop_requested && now >= next_sample_at) {
            const process_sample current = read_process_tree_sample(fp_proc->pid);
            if (have_previous) {
                const unsigned long cpu_delta = current.cpu_ticks >= previous.cpu_ticks
                                                    ? current.cpu_ticks - previous.cpu_ticks
                                                    : std::numeric_limits<unsigned long>::max();
                const unsigned long rss_delta =
                    absolute_difference(current.resident_pages, previous.resident_pages);
                const unsigned long virtual_delta =
                    absolute_difference(current.virtual_bytes, previous.virtual_bytes);
                const bool idle_state = current.state != 'R' && current.state != 'D';
                const bool stable = idle_state && cpu_delta <= max_cpu_ticks_per_sample &&
                                    rss_delta <= max_rss_delta_pages && virtual_delta <= page_size &&
                                    current.process_identities == previous.process_identities;

                d_printf(1,
                         "quiescence sample: processes=%zu state=%c cpu_delta=%lu "
                         "rss_delta=%lu pages vm_delta=%lu bytes stable=%s\n",
                         current.process_count, current.state, cpu_delta, rss_delta, virtual_delta,
                         stable ? "yes" : "no");

                if (stable) {
                    if (!stability_window_started) {
                        stable_since = now;
                        stability_window_started = true;
                    }
                    if (now - stable_since >= std::chrono::seconds(quiet_window_seconds)) {
                        d_printf(1, "quiescence reached after %lldms\n",
                                 static_cast<long long>(
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - started)
                                         .count()));
                        stop_requested = true;
                    }
                } else {
                    stability_window_started = false;
                }
            }
            previous = current;
            have_previous = true;
            next_sample_at = now + sample_interval;
        }

        if (!stop_requested &&
            now - started >= std::chrono::seconds(quiescence_timeout_seconds)) {
            std::fprintf(stderr,
                         "%s: warning - quiescence timeout after %u seconds; "
                         "capturing current state\n",
                         program_name, quiescence_timeout_seconds);
            stop_requested = true;
        }
        if (!stop_requested && exiting != 0) {
            d_printf(1, "interrupt received; capturing current state\n");
            stop_requested = true;
        }
        if (stop_requested && (fp_proc->flags & FP_STOPPING) == 0) {
            fp_proc->flags |= FP_STOPPING;
            root_stopped_by_us = true;
            if (::kill(fp_proc->pid, SIGSTOP) < 0) {
                abend("could not stop target: kill(%d, SIGSTOP) failed: %s\n", fp_proc->pid,
                      std::strerror(errno));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool read_process_state(pid_t pid, char* state) {
    std::vector<std::string> fields;
    if (!read_proc_stat_fields(pid, nullptr, &fields) || fields.empty() || fields[0].empty()) {
        return false;
    }
    *state = fields[0][0];
    return true;
}

bool write_control_file(const std::string& path, const char* value, std::string* reason) {
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        *reason = "open " + path + " failed: " + std::strerror(errno);
        return false;
    }
    const std::size_t length = std::strlen(value);
    const ssize_t written = write(fd, value, length);
    const int saved_errno = errno;
    if (close(fd) < 0 && written == static_cast<ssize_t>(length)) {
        *reason = "close " + path + " failed: " + std::strerror(errno);
        return false;
    }
    if (written != static_cast<ssize_t>(length)) {
        *reason = "write " + path + " failed: " + std::strerror(saved_errno);
        return false;
    }
    return true;
}

bool read_cgroup_frozen(const std::string& cgroup_path, bool* frozen) {
    std::ifstream events(cgroup_path + "/cgroup.events");
    std::string key;
    unsigned value = 0;
    while (events >> key >> value) {
        if (key == "frozen") {
            *frozen = value != 0;
            return true;
        }
    }
    return false;
}

bool read_unified_cgroup_path(pid_t pid, std::string* cgroup_path, std::string* reason) {
    std::ifstream membership("/proc/" + std::to_string(pid) + "/cgroup");
    std::string line;
    while (std::getline(membership, line)) {
        if (line.rfind("0::", 0) != 0) {
            continue;
        }
        const std::string relative = line.substr(3);
        if (relative.empty() || relative[0] != '/') {
            *reason = "invalid unified cgroup membership";
            return false;
        }
        std::error_code error;
        const std::filesystem::path root = std::filesystem::canonical("/sys/fs/cgroup", error);
        if (error) {
            *reason = "cgroup v2 is not mounted at /sys/fs/cgroup";
            return false;
        }
        const std::filesystem::path candidate =
            std::filesystem::weakly_canonical(root.string() + relative, error);
        if (error || (candidate != root && candidate.string().rfind(root.string() + "/", 0) != 0)) {
            *reason = "target cgroup path could not be resolved safely";
            return false;
        }
        *cgroup_path = candidate.string();
        return true;
    }
    *reason = "target has no cgroup v2 membership";
    return false;
}

bool add_cgroup_processes(const std::filesystem::path& directory, std::set<pid_t>* members) {
    std::ifstream processes(directory / "cgroup.procs");
    long value = 0;
    while (processes >> value) {
        if (value > 0 && value <= std::numeric_limits<pid_t>::max()) {
            members->insert(static_cast<pid_t>(value));
        }
    }
    return processes.eof();
}

bool read_cgroup_subtree_processes(const std::string& cgroup_path, std::set<pid_t>* members,
                                   std::string* reason) {
    members->clear();
    if (!add_cgroup_processes(cgroup_path, members)) {
        *reason = "could not read " + cgroup_path + "/cgroup.procs";
        return false;
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        cgroup_path, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        *reason = "could not enumerate the target cgroup subtree: " + error.message();
        return false;
    }
    while (iterator != end) {
        if (iterator->is_directory(error) && !error &&
            !add_cgroup_processes(iterator->path(), members)) {
            *reason = "could not read " + (iterator->path() / "cgroup.procs").string();
            return false;
        }
        error.clear();
        iterator.increment(error);
        if (error) {
            *reason = "could not enumerate the target cgroup subtree: " + error.message();
            return false;
        }
    }
    return true;
}

bool validate_launch_cgroup(const std::string& supplied_path, std::string* resolved_path,
                            std::string* reason) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical("/sys/fs/cgroup", error);
    if (error) {
        *reason = "cgroup v2 is not mounted at /sys/fs/cgroup";
        return false;
    }
    const std::filesystem::path candidate = std::filesystem::canonical(supplied_path, error);
    if (error || (candidate != root && candidate.string().rfind(root.string() + "/", 0) != 0)) {
        *reason = "launch cgroup must be an existing directory below /sys/fs/cgroup";
        return false;
    }
    if (access((candidate / "cgroup.procs").c_str(), W_OK) < 0 ||
        access((candidate / "cgroup.freeze").c_str(), W_OK) < 0) {
        *reason = "launch cgroup membership and freezer controls must be writable";
        return false;
    }
    bool frozen = false;
    if (!read_cgroup_frozen(candidate.string(), &frozen) || frozen) {
        *reason = "launch cgroup must be readable and initially thawed";
        return false;
    }
    std::set<pid_t> members;
    if (!read_cgroup_subtree_processes(candidate.string(), &members, reason)) {
        return false;
    }
    if (!members.empty()) {
        *reason = "launch cgroup must be empty so unrelated processes cannot be frozen";
        return false;
    }
    *resolved_path = candidate.string();
    return true;
}

bool thaw_cgroup() {
    if (frozen_cgroup_path.empty()) {
        return true;
    }
    std::string reason;
    if (!write_control_file(frozen_cgroup_path + "/cgroup.freeze", "0", &reason)) {
        std::fprintf(stderr, "%s: warning - could not thaw cgroup %s: %s\n", program_name,
                     frozen_cgroup_path.c_str(), reason.c_str());
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        bool frozen = true;
        if (read_cgroup_frozen(frozen_cgroup_path, &frozen) && !frozen) {
            frozen_cgroup_path.clear();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr,
                         "%s: warning - cgroup %s did not confirm frozen 0; "
                         "manual recovery may be required\n",
                         program_name, frozen_cgroup_path.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool try_freeze_cgroup(pid_t root_pid, std::set<pid_t>* frozen_tree, std::string* reason) {
    std::string cgroup_path;
    if (!read_unified_cgroup_path(root_pid, &cgroup_path, reason)) {
        return false;
    }
    const std::string freeze_file = cgroup_path + "/cgroup.freeze";
    if (access(freeze_file.c_str(), W_OK) < 0) {
        *reason = freeze_file + " is not writable: " + std::strerror(errno);
        return false;
    }

    bool already_frozen = false;
    if (!read_cgroup_frozen(cgroup_path, &already_frozen)) {
        *reason = "could not read " + cgroup_path + "/cgroup.events";
        return false;
    }
    if (already_frozen) {
        *reason = "target cgroup was already frozen; refusing to assume ownership";
        return false;
    }

    const std::set<pid_t> process_tree = discover_process_tree(root_pid);
    std::set<pid_t> cgroup_processes;
    if (!read_cgroup_subtree_processes(cgroup_path, &cgroup_processes, reason)) {
        return false;
    }
    if (cgroup_processes != process_tree) {
        *reason = "target cgroup is not dedicated to the measured process tree";
        return false;
    }

    if (!write_control_file(freeze_file, "1", reason)) {
        return false;
    }
    frozen_cgroup_path = cgroup_path;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        bool frozen = false;
        if (read_cgroup_frozen(cgroup_path, &frozen) && frozen) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *reason = "timed out waiting for cgroup.events to report frozen 1";
            if (!thaw_cgroup()) {
                *reason += "; thaw also failed for " + frozen_cgroup_path;
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::set<pid_t> stabilized_tree = discover_process_tree(root_pid);
    cgroup_processes.clear();
    const bool read_stabilized_cgroup =
        read_cgroup_subtree_processes(cgroup_path, &cgroup_processes, reason);
    if (!read_stabilized_cgroup || cgroup_processes != stabilized_tree) {
        if (reason->empty()) {
            *reason = "cgroup membership changed while freezing";
        }
        if (!thaw_cgroup()) {
            *reason += "; thaw also failed for " + frozen_cgroup_path;
        }
        return false;
    }
    *frozen_tree = stabilized_tree;
    selected_freeze_backend = "cgroup";
    selected_cgroup_path = cgroup_path;
    d_printf(1, "freeze backend: cgroup (%s), %zu processes\n", cgroup_path.c_str(),
             frozen_tree->size());
    return true;
}

int open_process_pidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

int signal_stopped_process(const stopped_process& process, int signal_number) {
#ifdef SYS_pidfd_send_signal
    if (process.pidfd >= 0) {
        return static_cast<int>(
            syscall(SYS_pidfd_send_signal, process.pidfd, signal_number, nullptr, 0));
    }
#endif
    unsigned long current_start_time = 0;
    if (!read_process_start_time(process.pid, &current_start_time) ||
        current_start_time != process.start_time) {
        errno = ESRCH;
        return -1;
    }
    return ::kill(process.pid, signal_number);
}

bool resume_stopped_descendants() {
    bool all_resumed = true;
    for (auto entry = descendants_stopped_by_us.begin();
         entry != descendants_stopped_by_us.end();) {
        const stopped_process& process = entry->second;
        if (signal_stopped_process(process, SIGCONT) < 0 && errno != ESRCH) {
            std::fprintf(stderr, "%s: warning - could not resume descendant %d: %s\n",
                         program_name, process.pid, std::strerror(errno));
            all_resumed = false;
            ++entry;
            continue;
        }
        if (process.pidfd >= 0) {
            close(process.pidfd);
        }
        entry = descendants_stopped_by_us.erase(entry);
    }
    return all_resumed;
}

std::set<pid_t> freeze_process_tree(pid_t root_pid) {
    constexpr int max_tree_stabilization_rounds = 10;
    constexpr auto stop_timeout = std::chrono::seconds(2);

    if (requested_freeze_mode != freeze_mode::signals) {
        std::set<pid_t> cgroup_tree;
        std::string reason;
        if (try_freeze_cgroup(root_pid, &cgroup_tree, &reason)) {
            return cgroup_tree;
        }
        if (!frozen_cgroup_path.empty()) {
            abend("cgroup freeze failed and the target remains frozen at %s: %s\n",
                  frozen_cgroup_path.c_str(), reason.c_str());
        }
        if (requested_freeze_mode == freeze_mode::cgroup) {
            abend("cgroup freeze requested but unavailable: %s\n", reason.c_str());
        }
        d_printf(1, "freeze backend: cgroup unavailable (%s); using signals\n",
                 reason.c_str());
    }

    for (int round = 0; round < max_tree_stabilization_rounds; ++round) {
        const std::set<pid_t> tree = discover_process_tree(root_pid);
        for (pid_t pid : tree) {
            if (pid == root_pid) {
                continue;
            }
            char state = '?';
            if (!read_process_state(pid, &state)) {
                continue;
            }
            if (state == 'T' || state == 't') {
                continue;
            }
            const auto existing_stop = descendants_stopped_by_us.find(pid);
            if (existing_stop != descendants_stopped_by_us.end()) {
                if (signal_stopped_process(existing_stop->second, SIGSTOP) < 0 && errno != ESRCH) {
                    abend("could not re-stop descendant %d: %s\n", pid, std::strerror(errno));
                }
                continue;
            }
            unsigned long start_time = 0;
            if (!read_process_start_time(pid, &start_time)) {
                continue;
            }
            const int pidfd = open_process_pidfd(pid);
            stopped_process process {pid, start_time, pidfd};
            if (signal_stopped_process(process, SIGSTOP) < 0) {
                const int saved_errno = errno;
                if (pidfd >= 0) {
                    close(pidfd);
                }
                if (saved_errno == ESRCH) {
                    continue;
                }
                abend("could not stop descendant %d: %s\n", pid,
                      std::strerror(saved_errno));
            }
            descendants_stopped_by_us.emplace(pid, process);
        }

        const auto deadline = std::chrono::steady_clock::now() + stop_timeout;
        for (;;) {
            bool all_stopped = true;
            for (pid_t pid : tree) {
                if (pid == root_pid) {
                    continue;
                }
                char state = '?';
                if (!read_process_state(pid, &state)) {
                    continue;
                }
                if (state != 'T' && state != 't') {
                    all_stopped = false;
                    break;
                }
            }
            if (all_stopped) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                abend("timed out while stopping the descendant process tree\n");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const std::set<pid_t> stabilized_tree = discover_process_tree(root_pid);
        if (stabilized_tree == tree) {
            selected_freeze_backend = "signals";
            selected_cgroup_path.clear();
            d_printf(1, "freeze backend: signals, %zu processes\n", tree.size());
            return tree;
        }
    }
    abend("process tree did not stabilize while freezing\n");
}

struct smaps_metrics {
    unsigned long size_kb = 0;
    unsigned long rss_kb = 0;
    unsigned long pss_kb = 0;
    unsigned long pss_anon_kb = 0;
    unsigned long pss_file_kb = 0;
    unsigned long pss_shmem_kb = 0;
    unsigned long shared_clean_kb = 0;
    unsigned long shared_dirty_kb = 0;
    unsigned long private_clean_kb = 0;
    unsigned long private_dirty_kb = 0;
    unsigned long private_hugetlb_kb = 0;
    unsigned long anonymous_kb = 0;
    unsigned long swap_kb = 0;
    unsigned long swap_pss_kb = 0;
    bool rss_available = false;
    bool pss_available = false;
    bool pss_anon_available = false;
    bool pss_file_available = false;
    bool pss_shmem_available = false;
    bool shared_clean_available = false;
    bool shared_dirty_available = false;
    bool private_clean_available = false;
    bool private_dirty_available = false;
    bool private_hugetlb_available = false;
    bool swap_available = false;
    bool swap_pss_available = false;
};

void add_smaps_field(smaps_metrics* metrics, const std::string& key, unsigned long value) {
    if (key == "Size") {
        metrics->size_kb += value;
    } else if (key == "Rss") {
        metrics->rss_kb += value;
        metrics->rss_available = true;
    } else if (key == "Pss") {
        metrics->pss_kb += value;
        metrics->pss_available = true;
    } else if (key == "Pss_Anon") {
        metrics->pss_anon_kb += value;
        metrics->pss_anon_available = true;
    } else if (key == "Pss_File") {
        metrics->pss_file_kb += value;
        metrics->pss_file_available = true;
    } else if (key == "Pss_Shmem") {
        metrics->pss_shmem_kb += value;
        metrics->pss_shmem_available = true;
    } else if (key == "Shared_Clean") {
        metrics->shared_clean_kb += value;
        metrics->shared_clean_available = true;
    } else if (key == "Shared_Dirty") {
        metrics->shared_dirty_kb += value;
        metrics->shared_dirty_available = true;
    } else if (key == "Private_Clean") {
        metrics->private_clean_kb += value;
        metrics->private_clean_available = true;
    } else if (key == "Private_Dirty") {
        metrics->private_dirty_kb += value;
        metrics->private_dirty_available = true;
    } else if (key == "Private_Hugetlb") {
        metrics->private_hugetlb_kb += value;
        metrics->private_hugetlb_available = true;
    } else if (key == "Anonymous") {
        metrics->anonymous_kb += value;
    } else if (key == "Swap") {
        metrics->swap_kb += value;
        metrics->swap_available = true;
    } else if (key == "SwapPss") {
        metrics->swap_pss_kb += value;
        metrics->swap_pss_available = true;
    }
}

bool uss_available(const smaps_metrics& metrics) {
    return metrics.private_clean_available && metrics.private_dirty_available &&
           metrics.private_hugetlb_available;
}

void merge_smaps_metrics(smaps_metrics* total, const smaps_metrics& memory, bool first) {
    total->size_kb += memory.size_kb;
    total->rss_kb += memory.rss_kb;
    total->pss_kb += memory.pss_kb;
    total->pss_anon_kb += memory.pss_anon_kb;
    total->pss_file_kb += memory.pss_file_kb;
    total->pss_shmem_kb += memory.pss_shmem_kb;
    total->shared_clean_kb += memory.shared_clean_kb;
    total->shared_dirty_kb += memory.shared_dirty_kb;
    total->private_clean_kb += memory.private_clean_kb;
    total->private_dirty_kb += memory.private_dirty_kb;
    total->private_hugetlb_kb += memory.private_hugetlb_kb;
    total->anonymous_kb += memory.anonymous_kb;
    total->swap_kb += memory.swap_kb;
    total->swap_pss_kb += memory.swap_pss_kb;

#define MERGE_AVAILABILITY(field) \
    total->field = first ? memory.field : total->field && memory.field
    MERGE_AVAILABILITY(rss_available);
    MERGE_AVAILABILITY(pss_available);
    MERGE_AVAILABILITY(pss_anon_available);
    MERGE_AVAILABILITY(pss_file_available);
    MERGE_AVAILABILITY(pss_shmem_available);
    MERGE_AVAILABILITY(shared_clean_available);
    MERGE_AVAILABILITY(shared_dirty_available);
    MERGE_AVAILABILITY(private_clean_available);
    MERGE_AVAILABILITY(private_dirty_available);
    MERGE_AVAILABILITY(private_hugetlb_available);
    MERGE_AVAILABILITY(swap_available);
    MERGE_AVAILABILITY(swap_pss_available);
#undef MERGE_AVAILABILITY
}

void write_optional_kb(std::ostream& output, unsigned long value, bool available) {
    if (available) {
        output << value;
    }
}

bool read_smaps_file(const std::string& path, smaps_metrics* metrics) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, colon);
        const char* value_text = line.c_str() + colon + 1;
        char* end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(value_text, &end, 10);
        if (errno != 0 || end == value_text) {
            continue;
        }
        add_smaps_field(metrics, key, value);
    }
    return !input.bad() && metrics->rss_available && metrics->pss_available;
}

smaps_metrics read_smaps_metrics(pid_t pid) {
    smaps_metrics metrics;
    const std::string proc_root = "/proc/" + std::to_string(pid) + "/";
    if (!read_smaps_file(proc_root + "smaps_rollup", &metrics)) {
        metrics = smaps_metrics {};
        if (!read_smaps_file(proc_root + "smaps", &metrics)) {
            abend("could not read smaps data for process %d: %s\n", pid,
                  std::strerror(errno));
        }
    }
    return metrics;
}

struct process_memory_record {
    pid_t pid;
    pid_t parent;
    std::string command;
    smaps_metrics memory;
};

std::string sanitize_tsv(std::string value) {
    for (char& character : value) {
        if (character == '\t' || character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    return value;
}

void collect_process_tree_memory(pid_t root_pid, const std::set<pid_t>& tree) {
    std::vector<process_memory_record> records;
    smaps_metrics total;
    for (pid_t pid : tree) {
        std::vector<std::string> fields;
        std::string command;
        if (!read_proc_stat_fields(pid, &command, &fields) || fields.size() < 2) {
            if (pid == root_pid) {
                abend("target process %d disappeared during capture\n", root_pid);
            }
            continue;
        }
        char* end = nullptr;
        const long parent_value = std::strtol(fields[1].c_str(), &end, 10);
        const pid_t parent = end != fields[1].c_str() && *end == '\0'
                                 ? static_cast<pid_t>(parent_value)
                                 : 0;
        smaps_metrics memory = read_smaps_metrics(pid);
        // smaps_rollup does not expose Size on every supported kernel; stat's
        // vsize is the authoritative process-wide virtual address size.
        if (fields.size() > 20) {
            memory.size_kb = parse_unsigned(fields[20]) >> 10;
        }
        records.push_back(process_memory_record {pid, parent, command, memory});

        merge_smaps_metrics(&total, memory, records.size() == 1);
    }

    const unsigned long total_uss_kb =
        total.private_clean_kb + total.private_dirty_kb + total.private_hugetlb_kb;
    add_metric("Process tree count", records.size(), "Processes");
    add_metric("Process tree virtual size", total.size_kb, "KB");
    if (total.rss_available) add_metric("Process tree RSS", total.rss_kb, "KB");
    if (total.pss_available) add_metric("Process tree PSS", total.pss_kb, "KB");
    if (uss_available(total)) add_metric("Process tree USS", total_uss_kb, "KB");
    if (total.pss_anon_available)
        add_metric("Process tree anonymous", total.pss_anon_kb, "KB PSS");
    if (total.pss_file_available)
        add_metric("Process tree file-backed", total.pss_file_kb, "KB PSS");
    if (total.pss_shmem_available)
        add_metric("Process tree shared memory", total.pss_shmem_kb, "KB PSS");
    if (total.swap_available) add_metric("Process tree swap", total.swap_kb, "KB");
    if (total.swap_pss_available)
        add_metric("Process tree swap PSS", total.swap_pss_kb, "KB");

    const std::string output_name =
        output_path("process_tree_memory_" + std::to_string(root_pid) + ".tsv");
    std::string output_reason;
    std::ostringstream output;
    output << "pid\tppid\tcommand\tvsize_kb\trss_kb\tpss_kb\tuss_kb\tpss_anon_kb\t"
              "pss_file_kb\tpss_shmem_kb\tshared_clean_kb\tshared_dirty_kb\t"
              "private_clean_kb\tprivate_dirty_kb\tswap_kb\tswap_pss_kb\n";
    for (const process_memory_record& record : records) {
        const smaps_metrics& memory = record.memory;
        const unsigned long uss_kb =
            memory.private_clean_kb + memory.private_dirty_kb + memory.private_hugetlb_kb;
        output << record.pid << '\t' << record.parent << '\t' << sanitize_tsv(record.command)
               << '\t' << memory.size_kb << '\t';
        write_optional_kb(output, memory.rss_kb, memory.rss_available);
        output << '\t';
        write_optional_kb(output, memory.pss_kb, memory.pss_available);
        output << '\t';
        write_optional_kb(output, uss_kb, uss_available(memory));
        output << '\t';
        write_optional_kb(output, memory.pss_anon_kb, memory.pss_anon_available);
        output << '\t';
        write_optional_kb(output, memory.pss_file_kb, memory.pss_file_available);
        output << '\t';
        write_optional_kb(output, memory.pss_shmem_kb, memory.pss_shmem_available);
        output << '\t';
        write_optional_kb(output, memory.shared_clean_kb, memory.shared_clean_available);
        output << '\t';
        write_optional_kb(output, memory.shared_dirty_kb, memory.shared_dirty_available);
        output << '\t';
        write_optional_kb(output, memory.private_clean_kb, memory.private_clean_available);
        output << '\t';
        write_optional_kb(output, memory.private_dirty_kb, memory.private_dirty_available);
        output << '\t';
        write_optional_kb(output, memory.swap_kb, memory.swap_available);
        output << '\t';
        write_optional_kb(output, memory.swap_pss_kb, memory.swap_pss_available);
        output << '\n';
    }
    output << "TOTAL\t\t" << records.size() << " processes\t" << total.size_kb << '\t';
    write_optional_kb(output, total.rss_kb, total.rss_available);
    output << '\t';
    write_optional_kb(output, total.pss_kb, total.pss_available);
    output << '\t';
    write_optional_kb(output, total_uss_kb, uss_available(total));
    output << '\t';
    write_optional_kb(output, total.pss_anon_kb, total.pss_anon_available);
    output << '\t';
    write_optional_kb(output, total.pss_file_kb, total.pss_file_available);
    output << '\t';
    write_optional_kb(output, total.pss_shmem_kb, total.pss_shmem_available);
    output << '\t';
    write_optional_kb(output, total.shared_clean_kb, total.shared_clean_available);
    output << '\t';
    write_optional_kb(output, total.shared_dirty_kb, total.shared_dirty_available);
    output << '\t';
    write_optional_kb(output, total.private_clean_kb, total.private_clean_available);
    output << '\t';
    write_optional_kb(output, total.private_dirty_kb, total.private_dirty_available);
    output << '\t';
    write_optional_kb(output, total.swap_kb, total.swap_available);
    output << '\t';
    write_optional_kb(output, total.swap_pss_kb, total.swap_pss_available);
    output << '\n';
    if (!write_atomic_text(output_name, output.str(), &output_reason)) {
        abend("%s\n", output_reason.c_str());
    }
}

void run_snapshot_command(const std::string& executable, const std::vector<std::string>& arguments,
                          const std::string& output_name) {
    secure_temp_file temporary;
    std::string reason;
    if (!create_secure_temp(output_name, &temporary, &reason)) {
        abend("%s\n", reason.c_str());
    }
    const pid_t child = fork();
    if (child < 0) {
        discard_secure_temp(&temporary);
        abend("could not fork snapshot command %s: %s\n", executable.c_str(),
              std::strerror(errno));
    }
    if (child == 0) {
        if (dup2(temporary.fd, STDOUT_FILENO) < 0 || dup2(temporary.fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        dprintf(STDERR_FILENO, "exec %s failed: %s\n", executable.c_str(),
                std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        discard_secure_temp(&temporary);
        abend("waitpid failed for snapshot command %s: %s\n", executable.c_str(),
              std::strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        discard_secure_temp(&temporary);
        abend("snapshot command %s failed with status %d\n", executable.c_str(), status);
    }
    if (!commit_secure_temp(&temporary, output_name, &reason)) {
        abend("%s\n", reason.c_str());
    }
}

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool process_is_hotspot(pid_t pid) {
    char executable[4096];
    const std::string executable_link = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t length = readlink(executable_link.c_str(), executable, sizeof(executable) - 1);
    if (length > 0) {
        executable[length] = '\0';
        const std::string path(executable);
        const std::size_t slash = path.find_last_of('/');
        const std::string basename = slash == std::string::npos ? path : path.substr(slash + 1);
        if (basename == "java") {
            return true;
        }
    }

    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libjvm.so") != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct command_capture_result {
    int status = -1;
    bool timed_out = false;
    bool output_ready = false;
};

bool service_traced_target_during_probe(fp_prochandle* fp_proc) {
    fp_proc->status = 0;
    const pid_t result = waitpid(fp_proc->pid, &fp_proc->status, WNOHANG | WUNTRACED);
    if (result <= 0) {
        return true;
    }
    if (WIFEXITED(fp_proc->status) || WIFSIGNALED(fp_proc->status)) {
        fp_proc->flags &= static_cast<unsigned short>(~FP_LIVE);
        return false;
    }
    if (!WIFSTOPPED(fp_proc->status)) {
        return true;
    }
    fp_proc->cursig = WSTOPSIG(fp_proc->status);
    fp_proc->flags |= FP_STOPPED;
    // HotSpot's dynamic attach protocol uses SIGQUIT to wake the target. The
    // normal signal-preserving path forwards it promptly to a traced JVM.
    return resume_after_target_stop(fp_proc);
}

command_capture_result run_jcmd_capture(fp_prochandle* fp_proc, pid_t pid,
                                        const std::string& output_name) {
    command_capture_result result;
    secure_temp_file temporary;
    std::string reason;
    if (!create_secure_temp(output_name, &temporary, &reason)) {
        d_printf(1, "%s\n", reason.c_str());
        return result;
    }
    const auto finish = [&]() {
        result.output_ready = commit_secure_temp(&temporary, output_name, &reason);
        if (!result.output_ready) {
            d_printf(1, "%s\n", reason.c_str());
        }
        return result;
    };
    const auto abandon = [&]() {
        discard_secure_temp(&temporary);
        result.output_ready = false;
        return result;
    };
    const pid_t child = fork();
    if (child < 0) {
        d_printf(1, "could not fork jcmd: %s\n", std::strerror(errno));
        return abandon();
    }
    if (child == 0) {
        if (dup2(temporary.fd, STDOUT_FILENO) < 0 ||
            dup2(temporary.fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(temporary.fd);
        const std::string pid_text = std::to_string(pid);
        execlp("jcmd", "jcmd", pid_text.c_str(), "VM.native_memory", "summary", "scale=KB",
               static_cast<char*>(nullptr));
        dprintf(STDERR_FILENO, "exec jcmd failed: %s\n", std::strerror(errno));
        _exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        int status = 0;
        if (!service_traced_target_during_probe(fp_proc)) {
            (void)::kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            result.status = status;
            return finish();
        }
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            result.status = status;
            return finish();
        }
        if (waited < 0 && errno != EINTR) {
            return finish();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            (void)::kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            result.status = status;
            return finish();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::string read_text_file(const std::string& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool parse_nmt_value_kb(const std::string& line, const char* key, unsigned long* value) {
    const std::size_t position = line.find(key);
    if (position == std::string::npos) {
        return false;
    }
    const char* start = line.c_str() + position + std::strlen(key);
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(start, &end, 10);
    if (errno != 0 || end == start) {
        return false;
    }
    if (std::strncmp(end, "KB", 2) != 0 && *end != ',' && *end != ')') {
        return false;
    }
    *value = parsed;
    return true;
}

std::size_t write_nmt_tsv(pid_t pid, const std::string& raw_output,
                          const runtime_capture_metadata& metadata) {
    const std::string output_name = output_path("jvm_nmt_" + std::to_string(pid) + ".tsv");
    std::string reason;
    std::ostringstream output;
    output << "pid\truntime\truntime_version\ttimestamp_ns\tduration_ns\tmethod\tforced_gc\t"
              "category\treserved_kb\tcommitted_kb\tunit\n";
    std::istringstream lines(raw_output);
    std::string line;
    std::size_t categories = 0;
    while (std::getline(lines, line)) {
        const std::string trimmed = trim_copy(line);
        std::string category;
        if (trimmed.rfind("Total:", 0) == 0) {
            category = "Total";
        } else if (!trimmed.empty() && trimmed[0] == '-') {
            const std::size_t parenthesis = trimmed.find('(');
            if (parenthesis != std::string::npos) {
                category = trim_copy(trimmed.substr(1, parenthesis - 1));
            }
        }
        unsigned long reserved_kb = 0;
        unsigned long committed_kb = 0;
        if (category.empty() || !parse_nmt_value_kb(trimmed, "reserved=", &reserved_kb) ||
            !parse_nmt_value_kb(trimmed, "committed=", &committed_kb)) {
            continue;
        }
        output << pid << '\t' << metadata.runtime << '\t'
               << sanitize_tsv(metadata.runtime_version) << '\t' << metadata.timestamp_ns << '\t'
               << metadata.duration_ns << '\t' << sanitize_tsv(metadata.method) << '\t'
               << (metadata.forced_gc ? "true" : "false") << '\t' << sanitize_tsv(category)
               << '\t' << reserved_kb << '\t' << committed_kb << '\t' << metadata.units << '\n';
        ++categories;
    }
    if (!write_atomic_text(output_name, output.str(), &reason)) {
        d_printf(1, "%s\n", reason.c_str());
        return 0;
    }
    return categories;
}

void capture_jvm_nmt(fp_prochandle* fp_proc, pid_t pid, unsigned long expected_start_time) {
    const std::string raw_name = output_path("jvm_nmt_" + std::to_string(pid) + ".txt");
    d_printf(1, "JVM detected in process %d; querying Native Memory Tracking\n", pid);
    user_message("HotSpot JVM detected in process %d; collecting Native Memory Tracking\n", pid);
    runtime_capture_metadata metadata;
    unsigned long current_start_time = 0;
    if (!read_process_start_time(pid, &current_start_time) ||
        current_start_time != expected_start_time) {
        metadata.status = "process_changed";
        jvm_capture_status[pid] = metadata;
        std::fprintf(stderr,
                     "%s: warning - JVM process %d changed identity before jcmd; skipping\n",
                     program_name, pid);
        return;
    }
    metadata.timestamp_ns = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto started = std::chrono::steady_clock::now();
    const command_capture_result result = run_jcmd_capture(fp_proc, pid, raw_name);
    metadata.duration_ns = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    const std::string output = result.output_ready ? read_text_file(raw_name) : std::string {};
    if (result.timed_out) {
        metadata.status = "timeout";
        jvm_capture_status[pid] = metadata;
        std::fprintf(stderr, "%s: warning - jcmd timed out for JVM %d\n", program_name, pid);
        return;
    }
    if (output.find("Native memory tracking is not enabled") != std::string::npos) {
        metadata.status = "nmt_not_enabled";
        jvm_capture_status[pid] = metadata;
        d_printf(1, "JVM %d: Native Memory Tracking is not enabled\n", pid);
        return;
    }
    if (result.status < 0 || !WIFEXITED(result.status) || WEXITSTATUS(result.status) != 0) {
        metadata.status = "jcmd_failed";
        jvm_capture_status[pid] = metadata;
        std::fprintf(stderr, "%s: warning - jcmd failed for JVM %d; see %s\n", program_name,
                     pid, raw_name.c_str());
        return;
    }
    const std::size_t categories = write_nmt_tsv(pid, output, metadata);
    if (categories == 0) {
        metadata.status = "unparsed";
        jvm_capture_status[pid] = metadata;
        std::fprintf(stderr, "%s: warning - jcmd output for JVM %d had no NMT categories\n",
                     program_name, pid);
        return;
    }
    metadata.status = "captured";
    jvm_capture_status[pid] = metadata;
    d_printf(1, "JVM %d: captured %zu NMT categories\n", pid, categories);
}

bool capture_runtime_snapshots(fp_prochandle* fp_proc) {
    if (!jcmd_enabled) {
        return false;
    }
    const auto discover_jvms = [&]() {
        std::set<std::pair<pid_t, unsigned long>> result;
        for (pid_t pid : discover_process_tree(fp_proc->pid)) {
            unsigned long start_time = 0;
            if (process_is_hotspot(pid) && read_process_start_time(pid, &start_time)) {
                result.emplace(pid, start_time);
            }
        }
        return result;
    };
    const std::set<std::pair<pid_t, unsigned long>> jvms = discover_jvms();
    if (jvms.empty()) {
        return false;
    }
    if (root_externally_stopped) {
        for (const auto& identity : jvms) {
            runtime_capture_metadata metadata;
            metadata.status = "skipped_external_stop";
            jvm_capture_status[identity.first] = metadata;
        }
        return false;
    }

    // The quiescence/fixed-delay phase returns with the traced root stopped.
    // Runtime attachment requires a responsive target, so resume it during the
    // probes and settle the workload again afterwards.
    fp_proc->cursig = 0;
    continue_running(fp_proc);
    std::set<std::pair<pid_t, unsigned long>> attempted;
    for (int pass = 0; pass < 2; ++pass) {
        const auto current_jvms = pass == 0 ? jvms : discover_jvms();
        for (const auto& identity : current_jvms) {
            if (!attempted.insert(identity).second) continue;
            capture_jvm_nmt(fp_proc, identity.first, identity.second);
            reap_adopted_zombies(fp_proc->pid);
            if (root_externally_stopped) {
                break;
            }
            if ((fp_proc->flags & FP_LIVE) == 0) {
                if (!retarget_to_adopted_descendant(fp_proc)) {
                    return true;
                }
                break;
            }
        }
    }
    if (root_externally_stopped || (fp_proc->flags & FP_LIVE) == 0) {
        return true;
    }
    if (wait_time == -1) {
        continue_until_quiescent(fp_proc);
    } else {
        continue_until_elapsed(fp_proc, 1);
    }
    return true;
}

void write_capture_metadata(pid_t root_pid) {
    const std::string output_name =
        output_path("capture_metadata_" + std::to_string(root_pid) + ".tsv");
    std::string reason;
    std::ostringstream output;
    output << "key\tpid\tvalue\n";
    output << "freeze_backend\t" << root_pid << '\t' << selected_freeze_backend << '\n';
    if (original_launcher_pid > 0 && original_launcher_pid != root_pid) {
        output << "launcher_pid\t" << root_pid << '\t' << original_launcher_pid << '\n';
    }
    if (!selected_cgroup_path.empty()) {
        output << "cgroup_path\t" << root_pid << '\t' << sanitize_tsv(selected_cgroup_path)
               << '\n';
    }
    for (const auto& capture : jvm_capture_status) {
        const runtime_capture_metadata& metadata = capture.second;
        output << "jvm_nmt\t" << capture.first << '\t' << metadata.status << '\n';
        output << "jvm_runtime\t" << capture.first << '\t' << metadata.runtime << '\n';
        if (!metadata.runtime_version.empty()) {
            output << "jvm_runtime_version\t" << capture.first << '\t'
                   << sanitize_tsv(metadata.runtime_version) << '\n';
        }
        if (metadata.timestamp_ns != 0) {
            output << "jvm_timestamp_ns\t" << capture.first << '\t' << metadata.timestamp_ns
                   << '\n';
        }
        output << "jvm_duration_ns\t" << capture.first << '\t' << metadata.duration_ns << '\n';
        output << "jvm_method\t" << capture.first << '\t' << sanitize_tsv(metadata.method) << '\n';
        output << "jvm_units\t" << capture.first << '\t' << metadata.units << '\n';
        output << "jvm_forced_gc\t" << capture.first << '\t'
               << (metadata.forced_gc ? "true" : "false") << '\n';
    }
    if (!write_atomic_text(output_name, output.str(), &reason)) {
        abend("%s\n", reason.c_str());
    }
}

void collect_memftprt(fp_prochandle* fp_proc) {
    const std::set<pid_t> process_tree = freeze_process_tree(fp_proc->pid);
    if (selected_freeze_backend == "cgroup") {
        std::set<pid_t> cgroup_processes;
        std::string reason;
        if (!read_cgroup_subtree_processes(selected_cgroup_path, &cgroup_processes, &reason) ||
            cgroup_processes != process_tree) {
            (void)thaw_cgroup();
            abend("cgroup membership changed before capture; refusing to measure unrelated "
                  "processes%s%s\n",
                  reason.empty() ? "" : ": ", reason.c_str());
        }
    }
    collect_process_tree_memory(fp_proc->pid, process_tree);

    proc_stats stats {};
    char dirname[80];
    std::snprintf(dirname, sizeof(dirname), "/proc/%d", fp_proc->pid);

    char* status = read_file(dirname, "status");
    (void)read_kb_field(status, "VmPeak:", &stats.status_vmpeak);
    (void)read_kb_field(status, "VmHWM:", &stats.status_vmhwm);
    (void)read_kb_field(status, "VmPTE:", &stats.status_vmpte);
    if (!read_kb_field(status, "VmSize:", &stats.status_vmsize)) {
        std::free(status);
        abend("VmSize not found in status data\n");
    }
    if (!read_kb_field(status, "VmRSS:", &stats.status_vmrss)) {
        std::free(status);
        abend("VmRSS not found in status data\n");
    }
    (void)read_kb_field(status, "VmLck:", &stats.status_vmlock);
    (void)read_kb_field(status, "VmData:", &stats.status_vmdata);
    (void)read_kb_field(status, "VmStk:", &stats.status_vmstack);
    (void)read_kb_field(status, "VmExe:", &stats.status_vmexe);
    (void)read_kb_field(status, "VmLib:", &stats.status_vmlib);
    std::free(status);

    char* statm = read_file(dirname, "statm");
    const int statm_fields = std::sscanf(statm, "%ld %ld %ld %ld %ld %ld %ld", &stats.statm_size,
                                         &stats.statm_resident, &stats.statm_share, &stats.statm_trs,
                                         &stats.statm_lrs, &stats.statm_drs, &stats.statm_dt);
    std::free(statm);
    fp_assert(statm_fields == 7, "read_statm: sccanf failed: num = %d, expected 7\n",
              statm_fields);

    char* stat_text = read_file(dirname, "stat");
    const char* after_name = std::strrchr(stat_text, ')');
    if (after_name == nullptr) {
        std::free(stat_text);
        abend("')' not found in stat data\n");
    }
    std::istringstream fields_stream(after_name + 2);
    std::vector<std::string> fields;
    std::string field;
    while (fields_stream >> field) {
        fields.push_back(field);
    }
    std::free(stat_text);
    // fields[0] is /proc/<pid>/stat field 3 (state).
    fp_assert(fields.size() > 33, "read_stat: expected at least 36 fields, found %zu\n",
              fields.size() + 2);
    stats.stat_minflt = parse_unsigned(fields[7]);
    stats.stat_majflt = parse_unsigned(fields[9]);
    stats.stat_vsize = parse_unsigned(fields[20]);
    stats.stat_rss = parse_signed(fields[21]);
    stats.stat_rlim = parse_unsigned(fields[22]);
    stats.stat_nswap = parse_unsigned(fields[33]);

    const long page_size_bytes_value = sysconf(_SC_PAGESIZE);
    const ulong_t page_size_kb = page_size_bytes_value > 0
                                     ? static_cast<ulong_t>(page_size_bytes_value) / 1024UL
                                     : 4UL;
    add_metric("Share - top", static_cast<ulong_t>(stats.statm_share) * page_size_kb, "KB");
    add_metric("RSS - ps", static_cast<ulong_t>(stats.status_vmrss), "KB");
    add_metric("Minor Faults  - ps", stats.stat_minflt, "Faults");
    add_metric("Major Faults - ps ", stats.stat_majflt, "Faults");
    add_metric("VSize - ps", stats.stat_vsize >> 10, "KB");
    add_metric("RSS - /proc/pid/stat", static_cast<ulong_t>(stats.stat_rss), "Pages");
    add_metric("RSS - /proc/pid/stat", static_cast<ulong_t>(stats.stat_rss) * page_size_kb, "KB");
    add_metric("Current limit on the RSS", stats.stat_rlim >> 10, "KB");
    add_metric("Number of pages swapped", stats.stat_nswap, "Pages");
    add_metric("Shared pages - statm", static_cast<ulong_t>(stats.statm_share), "Pages");
    add_metric("Data/Stack - statm", static_cast<ulong_t>(stats.statm_drs), "Pages");
    add_metric("Dirty pages - statm", static_cast<ulong_t>(stats.statm_dt), "Pages");
    add_metric("Resident set size - statm", static_cast<ulong_t>(stats.statm_resident), "Pages");
    add_metric("Text code - statm", static_cast<ulong_t>(stats.statm_trs), "Pages");
    add_metric("Library - statm", static_cast<ulong_t>(stats.statm_lrs), "Pages");
    add_metric("Total program size - statm", static_cast<ulong_t>(stats.statm_size), "Pages");
    add_metric("VmLck: Locked memory size", static_cast<ulong_t>(stats.status_vmlock), "KB");
    add_metric("VmLib: Shared library code size", static_cast<ulong_t>(stats.status_vmlib), "KB");
    add_metric("VmPeak: Peak VM size", static_cast<ulong_t>(stats.status_vmpeak), "KB");
    add_metric("VmData: Size of data", static_cast<ulong_t>(stats.status_vmdata), "KB");
    add_metric("VmStk: Size of stack", static_cast<ulong_t>(stats.status_vmstack), "KB");
    add_metric("VmExe: Size of text segments", static_cast<ulong_t>(stats.status_vmexe), "KB");
    add_metric("VmSize: Virtual memory size", static_cast<ulong_t>(stats.status_vmsize), "KB");
    add_metric("VmHWM: Peak RSS - HWM", static_cast<ulong_t>(stats.status_vmhwm), "KB");
    add_metric("VmPTE: Page table entries size", static_cast<ulong_t>(stats.status_vmpte), "KB");
    add_metric("The page size", page_size_kb, "KB");

    std::string pid = std::to_string(fp_proc->pid);
    const std::string maps_name = output_path("proccess_maps_" + pid + ".txt");
    const std::string ps_name = output_path("process_ps_" + pid + ".txt");
    const std::string top_name = output_path("process_top_" + pid + ".txt");
    run_snapshot_command("/bin/cat", {"/proc/" + pid + "/maps"}, maps_name);
    run_snapshot_command("/bin/ps",
                         {"-o", "pid,uid,state,rss,vsz,minflt,majflt,args", "-p", pid}, ps_name);
    run_snapshot_command("/usr/bin/top", {"-b", "-n", "1", "-p", pid}, top_name);
    write_capture_metadata(fp_proc->pid);
    if (selected_freeze_backend == "cgroup") {
        std::set<pid_t> cgroup_processes;
        std::string reason;
        if (!read_cgroup_subtree_processes(selected_cgroup_path, &cgroup_processes, &reason) ||
            cgroup_processes != process_tree) {
            (void)thaw_cgroup();
            abend("cgroup membership changed during capture%s%s\n",
                  reason.empty() ? "" : ": ", reason.c_str());
        }
    }
    if (!thaw_cgroup()) {
        abend("capture completed but cgroup thaw could not be confirmed; target cgroup is %s\n",
              frozen_cgroup_path.c_str());
    }
    if (!resume_stopped_descendants()) {
        abend("capture completed but one or more descendants could not be resumed\n");
    }
}

fp_prochandle* grab_process(pid_t pid) {
    d_printf(3, "grab_process: attempting to attach to %d\n", pid);
    if (pid == getpid()) {
        abend("cannot attach to self: %d\n", pid);
    }
    char state = '?';
    const bool was_stopped = read_process_state(pid, &state) && (state == 'T' || state == 't');
    if (ptrace(PTRACE_ATTACH, pid, nullptr, 0) < 0) {
        abend("grab_process: ptrace(PTRACE_ATTACH,%d) failed: %s\n", pid,
              std::strerror(errno));
    }
    expected_initial_stop_signal = SIGSTOP;
    root_stopped_by_us = false;
    root_externally_stopped = was_stopped;
    auto* handle = new fp_prochandle {pid, FP_GRABBED, 0, 0};
    return handle;
}

fp_prochandle* start_process(char** argv) {
    if (argv == nullptr || argv[0] == nullptr || argv[0][0] == '\0') {
        abend("bad arguments: no command name given\n");
    }
    if (!requested_launch_cgroup.empty()) {
        std::string resolved;
        std::string reason;
        if (!validate_launch_cgroup(requested_launch_cgroup, &resolved, &reason)) {
            abend("invalid launch cgroup %s: %s\n", requested_launch_cgroup.c_str(),
                  reason.c_str());
        }
        requested_launch_cgroup = resolved;
    }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0) {
        abend("could not enable child-subreaper mode: %s\n", std::strerror(errno));
    }
    const pid_t pid = fork();
    if (pid < 0) {
        abend("could not fork %s: %s\n", argv[0], std::strerror(errno));
    }
    if (pid == 0) {
        if (!requested_launch_cgroup.empty()) {
            std::string reason;
            const std::string self = std::to_string(getpid());
            if (!write_control_file(requested_launch_cgroup + "/cgroup.procs", self.c_str(),
                                    &reason)) {
                abend("could not join launch cgroup %s: %s\n",
                      requested_launch_cgroup.c_str(), reason.c_str());
            }
        }
        if (enable_jvm_nmt) {
            const char* existing = std::getenv("JAVA_TOOL_OPTIONS");
            std::string options = existing == nullptr ? "" : existing;
            if (!options.empty()) {
                options += ' ';
            }
            // HotSpot applies the last occurrence. Preserve an existing detail
            // request, but override an earlier "off" or malformed summary
            // setting when the user explicitly asks memftprt to enable NMT.
            options += options.find("-XX:NativeMemoryTracking=detail") != std::string::npos
                           ? "-XX:NativeMemoryTracking=detail"
                           : "-XX:NativeMemoryTracking=summary";
            if (setenv("JAVA_TOOL_OPTIONS", options.c_str(), 1) < 0) {
                abend("could not set JAVA_TOOL_OPTIONS: %s\n", std::strerror(errno));
            }
        }
        if (ptrace(PTRACE_TRACEME, 0, nullptr, 0) < 0) {
            abend("ptrace(PTRACE_TRACEME) failed: %s\n", std::strerror(errno));
        }
        execvp(argv[0], argv);
        abend("exec(%s) failed: %s\n", argv[0], std::strerror(errno));
    }
    expected_initial_stop_signal = SIGTRAP;
    root_stopped_by_us = false;
    root_externally_stopped = false;
    target_launched = true;
    original_launcher_pid = pid;
    return new fp_prochandle {pid, FP_CREATED, 0, 0};
}

void analyze_pid(pid_t pid) {
    initialize_library();
    target_process = grab_process(pid);
    acquire_output_lock(pid);
    announce_wait(pid, "attached to");
    if (wait_time == -1) {
        continue_until_quiescent(target_process);
    } else {
        continue_until_elapsed(target_process, static_cast<unsigned>(wait_time));
    }
    (void)capture_runtime_snapshots(target_process);
    user_message("freezing process tree and reading memory accounting\n");
    collect_memftprt(target_process);
    if (!quiet_mode) report();
    announce_completion(target_process->pid);
    release_process(target_process);
    target_process = nullptr;
    release_output_lock();
}

void analyze(char** argv) {
    initialize_library();
    target_process = start_process(argv);
    acquire_output_lock(target_process->pid);
    announce_wait(target_process->pid, "launched");
    if (wait_time == -1) {
        continue_until_quiescent(target_process);
    } else {
        continue_until_elapsed(target_process, static_cast<unsigned>(wait_time));
    }
    (void)capture_runtime_snapshots(target_process);
    user_message("freezing process tree and reading memory accounting\n");
    collect_memftprt(target_process);
    if (!quiet_mode) report();
    announce_completion(target_process->pid);
    if (kill_before_exit) {
        kill_process(target_process);
    } else {
        release_process(target_process);
    }
    target_process = nullptr;
    release_output_lock();
}

long parse_positive(const char* value, const char* option) {
    errno = 0;
    char* end = nullptr;
    const long result = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || result < 1) {
        cli_error("%s requires a positive integer, got '%s'", option, value);
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argv != nullptr && argv[0] != nullptr) {
        const char* slash = std::strrchr(argv[0], '/');
        program_name = slash == nullptr ? argv[0] : slash + 1;
    }
    if (argc < 2 || argv == nullptr || argv[0] == nullptr) {
        cli_error("a command or --pid PID is required");
    }

    pid_t pid = -1;
    int index = 1;
    while (index < argc && argv[index] != nullptr && argv[index][0] == '-') {
        if (std::strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        }
        if (std::strcmp(argv[index], "-h") == 0 ||
            std::strcmp(argv[index], "--help") == 0) {
            print_help(stdout);
            return 0;
        }
        if (std::strcmp(argv[index], "-V") == 0 ||
            std::strcmp(argv[index], "--version") == 0) {
            std::printf("memftprt %s\n", MEMFTPRT_VERSION);
            return 0;
        }
        if (std::strcmp(argv[index], "-p") == 0 ||
            std::strcmp(argv[index], "--pid") == 0) {
            if (++index >= argc) {
                cli_error("%s requires a PID", argv[index - 1]);
            }
            const long value = parse_positive(argv[index], argv[index - 1]);
            ++index;
            if (value > std::numeric_limits<pid_t>::max()) {
                cli_error("PID is outside the supported range");
            }
            pid = static_cast<pid_t>(value);
        } else if (std::strcmp(argv[index], "-t") == 0 ||
                   std::strcmp(argv[index], "--delay") == 0) {
            if (++index >= argc) {
                cli_error("%s requires a duration in seconds", argv[index - 1]);
            }
            const long value = parse_positive(argv[index], argv[index - 1]);
            ++index;
            if (value > std::numeric_limits<int>::max()) {
                cli_error("delay is outside the supported range");
            }
            wait_time = static_cast<int>(value);
        } else if (std::strcmp(argv[index], "-q") == 0 ||
                   std::strcmp(argv[index], "--stable-window") == 0) {
            if (++index >= argc) {
                cli_error("%s requires a duration in seconds", argv[index - 1]);
            }
            const unsigned long value =
                static_cast<unsigned long>(parse_positive(argv[index], argv[index - 1]));
            ++index;
            if (value > std::numeric_limits<unsigned>::max()) {
                cli_error("stable window is outside the supported range");
            }
            quiet_window_seconds = static_cast<unsigned>(value);
        } else if (std::strcmp(argv[index], "-m") == 0 ||
                   std::strcmp(argv[index], "--timeout") == 0) {
            if (++index >= argc) {
                cli_error("%s requires a duration in seconds", argv[index - 1]);
            }
            const unsigned long value =
                static_cast<unsigned long>(parse_positive(argv[index], argv[index - 1]));
            ++index;
            if (value > std::numeric_limits<unsigned>::max()) {
                cli_error("timeout is outside the supported range");
            }
            quiescence_timeout_seconds = static_cast<unsigned>(value);
        } else if (std::strcmp(argv[index], "-o") == 0 ||
                   std::strcmp(argv[index], "--output-dir") == 0) {
            if (++index >= argc) {
                cli_error("%s requires a directory", argv[index - 1]);
            }
            output_directory = argv[index++];
        } else if (std::strcmp(argv[index], "--quiet") == 0) {
            quiet_mode = true;
            ++index;
        } else if (std::strcmp(argv[index], "-v") == 0 ||
                   std::strcmp(argv[index], "--verbose") == 0) {
            ++debug_level;
            ++index;
        } else if (std::strcmp(argv[index], "--freeze") == 0) {
            if (++index >= argc) {
                cli_error("--freeze requires auto, cgroup, or signals");
            }
            if (std::strcmp(argv[index], "auto") == 0) {
                requested_freeze_mode = freeze_mode::automatic;
            } else if (std::strcmp(argv[index], "cgroup") == 0) {
                requested_freeze_mode = freeze_mode::cgroup;
            } else if (std::strcmp(argv[index], "signals") == 0) {
                requested_freeze_mode = freeze_mode::signals;
            } else {
                cli_error("unknown freeze mode '%s'; expected auto, cgroup, or signals",
                          argv[index]);
            }
            ++index;
        } else if (std::strcmp(argv[index], "--cgroup") == 0) {
            if (++index >= argc) {
                cli_error("--cgroup requires a delegated cgroup v2 path");
            }
            requested_launch_cgroup = argv[index++];
        } else if (std::strcmp(argv[index], "--no-jcmd") == 0) {
            jcmd_enabled = false;
            ++index;
        } else if (std::strcmp(argv[index], "--enable-jvm-nmt") == 0) {
            enable_jvm_nmt = true;
            ++index;
        } else {
            cli_error("unknown option '%s'", argv[index]);
        }
    }

    if (pid != -1) {
        if (enable_jvm_nmt || !requested_launch_cgroup.empty()) {
            cli_error("--enable-jvm-nmt and --cgroup are launch-only options");
        }
        if (index != argc) {
            cli_error("a command cannot be combined with --pid");
        }
        prepare_output_directory();
        analyze_pid(pid);
    } else {
        if (index >= argc) {
            cli_error("a command or --pid PID is required");
        }
        if (!requested_launch_cgroup.empty()) {
            if (requested_freeze_mode == freeze_mode::signals) {
                cli_error("--cgroup conflicts with --freeze signals");
            }
            requested_freeze_mode = freeze_mode::cgroup;
        }
        prepare_output_directory();
        analyze(argv + index);
    }
    return exiting == 0 ? 0 : 128 + static_cast<int>(exiting);
}
