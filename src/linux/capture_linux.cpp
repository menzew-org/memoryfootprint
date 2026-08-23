/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux capture orchestrator for memftprt.
 *
 * Owns target lifecycle, quiescence detection, consistent process-tree
 * freezing, Linux snapshot commands, and runtime-probe coordination. Generic
 * CLI, artifact storage, and report rendering live in separate modules.
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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

#include "../artifact_store.h"
#include "../capture.h"
#include "../cli.h"
#include "../memory_report.h"
#include "../runtime/hotspot_nmt.h"
#include "cgroup_utils.h"
#include "proc_memory.h"
#include "working_set.h"

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

namespace memftprt {
namespace {

constexpr unsigned short FP_CREATED = 0x01;
constexpr unsigned short FP_GRABBED = 0x02;
constexpr unsigned short FP_STOPPING = 0x04;
constexpr unsigned short FP_STOPPED = 0x40;
constexpr unsigned short FP_LIVE = FP_CREATED | FP_GRABBED;

enum class WaitOutcome {
    elapsed,
    interrupted,
    external_stop,
};

class CaptureInterrupted final : public std::exception {
public:
    const char* what() const noexcept override {
        return "capture interrupted";
    }
};

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
std::unique_ptr<ArtifactStore> artifact_store;

FreezeMode requested_freeze_mode = FreezeMode::automatic;
ReportFormat requested_report_format = ReportFormat::tsv;
bool jcmd_enabled = true;
bool enable_jvm_nmt = false;
bool mappings_enabled = false;
unsigned working_set_seconds = 0;
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
std::map<std::string, std::string> snapshot_capture_status;
std::map<pid_t, std::string> captured_smaps_sources;
std::optional<WorkingSetInfo> captured_working_set;
unsigned pidfd_successes = 0;
unsigned pidfd_failures = 0;

struct working_set_capture_state {
    std::map<pid_t, unsigned long> process_identities;
    std::map<pid_t, std::set<linux_working_set::MappingIdentity>> mapping_identities;
    WorkingSetInfo info;
    std::chrono::steady_clock::time_point started_monotonic;
    std::string freeze_backend;
    std::string cgroup_path;
};

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
                 selected_freeze_backend.c_str(), artifact_store->committed_count(),
                 artifact_store->directory().c_str());
}

std::string output_path(const std::string& filename) {
    return artifact_store->path(filename);
}

void acquire_output_lock(pid_t root_pid) {
    std::string reason;
    if (!artifact_store->acquire_lock(root_pid, &reason)) abend("%s\n", reason.c_str());

    // A marker from an earlier capture of a reused PID must stop advertising
    // the bundle as complete before this collector replaces any artifacts.
    const std::string marker = output_path("capture_complete_" +
                                           std::to_string(root_pid) + ".tsv");
    if (!artifact_store->remove_file(marker, true, &reason)) abend("%s\n", reason.c_str());
}

void release_output_lock() {
    if (artifact_store != nullptr) artifact_store->release_lock();
}

void cleanup_capture_target() {
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
    release_output_lock();
}

bool abandon_interrupted_capture() {
    if (exiting == 0) return false;
    cleanup_capture_target();
    return true;
}

using secure_temp_file = ArtifactStore::TempFile;

void discard_secure_temp(secure_temp_file* temporary) {
    artifact_store->discard_temp(temporary);
}

bool create_secure_temp(const std::string& output_name, secure_temp_file* temporary,
                        std::string* reason) {
    return artifact_store->create_temp(output_name, temporary, reason);
}

bool commit_secure_temp(secure_temp_file* temporary, const std::string& output_name,
                        std::string* reason) {
    return artifact_store->commit_temp(temporary, output_name, reason);
}

bool write_atomic_text(const std::string& output_name, const std::string& contents,
                       std::string* reason) {
    return artifact_store->write_text(output_name, contents, reason);
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

WaitOutcome continue_until_elapsed(fp_prochandle* fp_proc, unsigned seconds) {
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
                    return exiting == 0 ? WaitOutcome::elapsed : WaitOutcome::interrupted;
                }
                if (!resume_after_target_stop(fp_proc)) {
                    return WaitOutcome::external_stop;
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

WaitOutcome continue_until_quiescent(fp_prochandle* fp_proc) {
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
    WaitOutcome stop_outcome = WaitOutcome::elapsed;
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
                    return stop_outcome;
                }
                if (!resume_after_target_stop(fp_proc)) {
                    return WaitOutcome::external_stop;
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
            d_printf(1, "interrupt received; stopping target for cleanup\n");
            stop_outcome = WaitOutcome::interrupted;
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

bool read_cgroup_subtree_processes(const std::string& cgroup_path, std::set<pid_t>* members,
                                   std::string* reason) {
    return linux_cgroup::read_subtree_processes(cgroup_path, members, reason);
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

    if (requested_freeze_mode != FreezeMode::signals) {
        std::set<pid_t> cgroup_tree;
        std::string reason;
        if (try_freeze_cgroup(root_pid, &cgroup_tree, &reason)) {
            return cgroup_tree;
        }
        if (!frozen_cgroup_path.empty()) {
            abend("cgroup freeze failed and the target remains frozen at %s: %s\n",
                  frozen_cgroup_path.c_str(), reason.c_str());
        }
        if (requested_freeze_mode == FreezeMode::cgroup) {
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
            if (pidfd >= 0) {
                ++pidfd_successes;
            } else {
                ++pidfd_failures;
            }
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

using linux_procfs::SmapsMetrics;

struct process_memory_record {
    pid_t pid;
    pid_t parent;
    std::string command;
    SmapsMetrics memory;
};

OptionalKb optional_kb(unsigned long value, bool available) {
    return OptionalKb {value, available};
}

void populate_memory_values(MemoryValues* result, const SmapsMetrics& memory) {
    result->vsize_kb = memory.size_kb;
    result->rss_kb = optional_kb(memory.rss_kb, memory.rss_available);
    result->pss_kb = optional_kb(memory.pss_kb, memory.pss_available);
    result->uss_kb = optional_kb(linux_procfs::uss_kb(memory),
                                 linux_procfs::uss_available(memory));
    result->pss_anon_kb = optional_kb(memory.pss_anon_kb, memory.pss_anon_available);
    result->pss_file_kb = optional_kb(memory.pss_file_kb, memory.pss_file_available);
    result->pss_shmem_kb = optional_kb(memory.pss_shmem_kb, memory.pss_shmem_available);
    result->shared_clean_kb =
        optional_kb(memory.shared_clean_kb, memory.shared_clean_available);
    result->shared_dirty_kb =
        optional_kb(memory.shared_dirty_kb, memory.shared_dirty_available);
    result->private_clean_kb =
        optional_kb(memory.private_clean_kb, memory.private_clean_available);
    result->private_dirty_kb =
        optional_kb(memory.private_dirty_kb, memory.private_dirty_available);
    result->anonymous_kb = optional_kb(memory.anonymous_kb, memory.anonymous_available);
    result->referenced_kb = optional_kb(memory.referenced_kb, memory.referenced_available);
    result->lazy_free_kb = optional_kb(memory.lazy_free_kb, memory.lazy_free_available);
    result->locked_kb = optional_kb(memory.locked_kb, memory.locked_available);
    result->anon_huge_pages_kb =
        optional_kb(memory.anon_huge_pages_kb, memory.anon_huge_pages_available);
    result->shmem_pmd_mapped_kb =
        optional_kb(memory.shmem_pmd_mapped_kb, memory.shmem_pmd_mapped_available);
    result->file_pmd_mapped_kb =
        optional_kb(memory.file_pmd_mapped_kb, memory.file_pmd_mapped_available);
    result->shared_hugetlb_kb =
        optional_kb(memory.shared_hugetlb_kb, memory.shared_hugetlb_available);
    result->private_hugetlb_kb =
        optional_kb(memory.private_hugetlb_kb, memory.private_hugetlb_available);
    result->swap_kb = optional_kb(memory.swap_kb, memory.swap_available);
    result->swap_pss_kb = optional_kb(memory.swap_pss_kb, memory.swap_pss_available);
}

ProcessMemory make_process_memory(pid_t pid, pid_t parent, const std::string& command,
                                  const SmapsMetrics& memory) {
    ProcessMemory result;
    result.pid = pid;
    result.ppid = parent;
    result.command = command;
    populate_memory_values(&result, memory);
    return result;
}

MemoryMapping make_memory_mapping(pid_t pid, const linux_procfs::SmapsMapping& mapping,
                                  const std::string& category) {
    MemoryMapping result;
    result.pid = pid;
    result.start_address = mapping.start_address;
    result.end_address = mapping.end_address;
    result.offset = mapping.offset;
    result.inode = mapping.inode;
    result.permissions = mapping.permissions;
    result.device = mapping.device;
    result.pathname = mapping.pathname;
    result.category = category;
    result.kernel_page_size_kb =
        optional_kb(mapping.kernel_page_size_kb, mapping.kernel_page_size_available);
    result.mmu_page_size_kb =
        optional_kb(mapping.mmu_page_size_kb, mapping.mmu_page_size_available);
    result.thp_eligible = OptionalBool {mapping.thp_eligible, mapping.thp_eligible_available};
    result.vm_flags = mapping.vm_flags;
    populate_memory_values(&result, mapping.memory);
    return result;
}

std::string executable_path(pid_t pid) {
    char path[4096];
    const std::string link = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t length = readlink(link.c_str(), path, sizeof(path) - 1);
    if (length < 0) return {};
    path[length] = '\0';
    return path;
}

std::string pidfd_capture_status() {
    if (selected_freeze_backend != "signals" || pidfd_successes + pidfd_failures == 0) {
        return "not_applicable";
    }
    if (pidfd_failures == 0) return "used";
    if (pidfd_successes != 0) return "partial";
    return "unavailable";
}

MemoryReport build_process_tree_memory_report(
    pid_t root_pid, const std::set<pid_t>& tree, unsigned long long snapshot_timestamp_ns,
    const std::map<pid_t, linux_procfs::SmapsDetail>* supplied_details = nullptr) {
    std::vector<process_memory_record> records;
    std::vector<MemoryMapping> mapping_rows;
    struct category_accumulator {
        std::size_t count = 0;
        SmapsMetrics memory;
    };
    std::map<std::string, category_accumulator> category_totals;
    SmapsMetrics total;
    linux_procfs::SmapsReadBudget detailed_budget;
    captured_smaps_sources.clear();
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
        SmapsMetrics memory;
        std::string memory_reason;
        linux_procfs::SmapsDetail detail;
        if (mappings_enabled) {
            const linux_procfs::SmapsDetail* detail_source = &detail;
            if (supplied_details != nullptr) {
                const auto found = supplied_details->find(pid);
                if (found == supplied_details->end()) {
                    abend("missing frozen smaps detail for process %d\n", pid);
                }
                detail_source = &found->second;
            } else if (!linux_procfs::read_smaps_detail(
                           pid, &detail, &memory_reason, &detailed_budget,
                           []() { return exiting != 0; })) {
                if (exiting != 0) throw CaptureInterrupted {};
                abend("%s\n", memory_reason.c_str());
            }
            memory = detail_source->total;
            captured_smaps_sources[pid] = detail_source->process_totals_from_rollup
                                               ? "smaps+smaps_rollup"
                                               : "smaps";
            const std::string executable = executable_path(pid);
            for (const linux_procfs::SmapsMapping& mapping : detail_source->mappings) {
                const std::string category_name =
                    linux_procfs::classify_mapping(mapping, executable);
                category_accumulator& category = category_totals[category_name];
                linux_procfs::merge_smaps_metrics(&category.memory, mapping.memory,
                                                  category.count == 0);
                ++category.count;
                mapping_rows.push_back(make_memory_mapping(pid, mapping, category_name));
            }
        } else {
            std::string source;
            if (!linux_procfs::read_smaps_metrics(pid, &memory, &memory_reason, &source)) {
                abend("%s\n", memory_reason.c_str());
            }
            captured_smaps_sources[pid] = source;
        }
        // smaps_rollup does not expose Size on every supported kernel; stat's
        // vsize is the authoritative process-wide virtual address size.
        if (fields.size() > 20) {
            memory.size_kb = parse_unsigned(fields[20]) >> 10;
        }
        records.push_back(process_memory_record {pid, parent, command, memory});

        linux_procfs::merge_smaps_metrics(&total, memory, records.size() == 1);
    }

    const unsigned long total_uss_kb = linux_procfs::uss_kb(total);
    add_metric("Process tree count", records.size(), "Processes");
    add_metric("Process tree virtual size", total.size_kb, "KB");
    if (total.rss_available) add_metric("Process tree RSS", total.rss_kb, "KB");
    if (total.pss_available) add_metric("Process tree PSS", total.pss_kb, "KB");
    if (linux_procfs::uss_available(total)) add_metric("Process tree USS", total_uss_kb, "KB");
    if (total.pss_anon_available)
        add_metric("Process tree anonymous", total.pss_anon_kb, "KB PSS");
    if (total.pss_file_available)
        add_metric("Process tree file-backed", total.pss_file_kb, "KB PSS");
    if (total.pss_shmem_available)
        add_metric("Process tree shared memory", total.pss_shmem_kb, "KB PSS");
    if (total.swap_available) add_metric("Process tree swap", total.swap_kb, "KB");
    if (total.swap_pss_available)
        add_metric("Process tree swap PSS", total.swap_pss_kb, "KB");

    // Build one canonical report before serializing it. Keeping measurement and
    // presentation separate guarantees that every requested format describes
    // the exact same frozen process-tree snapshot.
    MemoryReport report;
    report.root_pid = root_pid;
    report.launcher_pid = original_launcher_pid;
    report.timestamp_ns = snapshot_timestamp_ns;
    report.freeze_backend = selected_freeze_backend;
    report.cgroup_path = selected_cgroup_path;
    report.pidfd_status = pidfd_capture_status();
    report.reference_scope = captured_working_set.has_value() ? "since_clear_refs"
                                                              : "kernel_current";
    report.mappings_captured = mappings_enabled;
    for (const auto& source : captured_smaps_sources) {
        report.smaps_sources[source.first] = source.second;
    }
    report.working_set = captured_working_set;
    for (const auto& capture : jvm_capture_status) {
        report.runtime_status[capture.first] = capture.second.status;
    }
    for (const process_memory_record& record : records) {
        report.processes.push_back(
            make_process_memory(record.pid, record.parent, record.command, record.memory));
    }
    report.total = make_process_memory(0, 0, std::to_string(records.size()) + " processes", total);
    report.mappings = std::move(mapping_rows);
    std::sort(report.mappings.begin(), report.mappings.end(),
              [](const MemoryMapping& left, const MemoryMapping& right) {
                  const unsigned long left_pss = left.pss_kb.available ? left.pss_kb.value : 0;
                  const unsigned long right_pss = right.pss_kb.available ? right.pss_kb.value : 0;
                  if (left_pss != right_pss) return left_pss > right_pss;
                  if (left.pid != right.pid) return left.pid < right.pid;
                  return left.start_address < right.start_address;
              });
    for (const auto& category : category_totals) {
        MappingCategory row;
        row.category = category.first;
        row.mapping_count = category.second.count;
        populate_memory_values(&row, category.second.memory);
        report.mapping_categories.push_back(row);
    }
    std::sort(report.mapping_categories.begin(), report.mapping_categories.end(),
              [](const MappingCategory& left, const MappingCategory& right) {
                  const unsigned long left_pss = left.pss_kb.available ? left.pss_kb.value : 0;
                  const unsigned long right_pss = right.pss_kb.available ? right.pss_kb.value : 0;
                  if (left_pss != right_pss) return left_pss > right_pss;
                  return left.category < right.category;
              });

    return report;
}

void write_memory_reports(const MemoryReport& report) {
    const pid_t root_pid = static_cast<pid_t>(report.root_pid);
    const auto write_report = [&](const std::string& filename, const std::string& contents) {
        std::string reason;
        if (!write_atomic_text(output_path(filename), contents, &reason)) {
            abend("%s\n", reason.c_str());
        }
    };
    if (requested_report_format == ReportFormat::tsv ||
        requested_report_format == ReportFormat::all) {
        write_report("process_tree_memory_" + std::to_string(root_pid) + ".tsv",
                     render_tsv(report));
        if (mappings_enabled) {
            write_report("memory_mappings_" + std::to_string(root_pid) + ".tsv",
                         render_mappings_tsv(report));
            write_report("memory_mapping_categories_" + std::to_string(root_pid) + ".tsv",
                         render_mapping_categories_tsv(report));
        }
    }
    if (requested_report_format == ReportFormat::json ||
        requested_report_format == ReportFormat::all) {
        write_report("memory_report_" + std::to_string(root_pid) + ".json",
                     render_json(report));
    }
    if (requested_report_format == ReportFormat::html ||
        requested_report_format == ReportFormat::all) {
        write_report("memory_report_" + std::to_string(root_pid) + ".html",
                     render_html(report));
    }
}

bool run_snapshot_command(const std::string& executable,
                          const std::vector<std::string>& arguments,
                          const std::string& output_name) {
    secure_temp_file temporary;
    std::string reason;
    if (!create_secure_temp(output_name, &temporary, &reason)) {
        std::fprintf(stderr, "%s: warning - optional snapshot %s unavailable: %s\n",
                     program_name, executable.c_str(), reason.c_str());
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        discard_secure_temp(&temporary);
        std::fprintf(stderr, "%s: warning - could not fork optional snapshot %s: %s\n",
                     program_name, executable.c_str(), std::strerror(errno));
        return false;
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
        execvp(executable.c_str(), argv.data());
        dprintf(STDERR_FILENO, "exec %s failed: %s\n", executable.c_str(),
                std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        if (exiting != 0) {
            (void)::kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            discard_secure_temp(&temporary);
            throw CaptureInterrupted {};
        }
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) {
            const int saved_errno = errno;
            (void)::kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            discard_secure_temp(&temporary);
            std::fprintf(stderr, "%s: warning - optional snapshot %s wait failed: %s\n",
                         program_name, executable.c_str(), std::strerror(saved_errno));
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)::kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            discard_secure_temp(&temporary);
            std::fprintf(stderr, "%s: warning - optional snapshot %s timed out\n",
                         program_name, executable.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        discard_secure_temp(&temporary);
        std::fprintf(stderr, "%s: warning - optional snapshot %s failed with status %d\n",
                     program_name, executable.c_str(), status);
        return false;
    }
    if (!commit_secure_temp(&temporary, output_name, &reason)) {
        std::fprintf(stderr, "%s: warning - optional snapshot %s was not committed: %s\n",
                     program_name, executable.c_str(), reason.c_str());
        return false;
    }
    return true;
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
    bool interrupted = false;
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
        if (exiting != 0) {
            result.interrupted = true;
            (void)::kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            result.status = status;
            return abandon();
        }
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

std::size_t write_nmt_tsv(pid_t pid, const std::string& raw_output,
                          const runtime_capture_metadata& metadata) {
    const std::string output_name = output_path("jvm_nmt_" + std::to_string(pid) + ".tsv");
    std::string reason;
    std::ostringstream output;
    output << "pid\truntime\truntime_version\ttimestamp_ns\tduration_ns\tmethod\tforced_gc\t"
              "category\treserved_kb\tcommitted_kb\tunit\n";
    const std::vector<hotspot_nmt::Category> categories =
        hotspot_nmt::parse_summary(raw_output);
    for (const hotspot_nmt::Category& category : categories) {
        output << pid << '\t' << metadata.runtime << '\t'
               << sanitize_tsv_field(metadata.runtime_version) << '\t' << metadata.timestamp_ns
               << '\t' << metadata.duration_ns << '\t' << sanitize_tsv_field(metadata.method)
               << '\t'
               << (metadata.forced_gc ? "true" : "false") << '\t'
               << sanitize_tsv_field(category.name) << '\t' << category.reserved_kb << '\t'
               << category.committed_kb << '\t' << metadata.units << '\n';
    }
    if (!write_atomic_text(output_name, output.str(), &reason)) {
        d_printf(1, "%s\n", reason.c_str());
        return 0;
    }
    return categories.size();
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
    if (result.interrupted) {
        metadata.status = "interrupted";
        jvm_capture_status[pid] = metadata;
        return;
    }
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
            if (exiting != 0) return true;
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
    if (exiting != 0) return true;
    if (wait_time == -1) {
        (void)continue_until_quiescent(fp_proc);
    } else {
        (void)continue_until_elapsed(fp_proc, 1);
    }
    return true;
}

unsigned long long system_timestamp_ns() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::map<pid_t, unsigned long> process_identities_for_tree(const std::set<pid_t>& tree) {
    std::map<pid_t, unsigned long> result;
    for (pid_t pid : tree) {
        unsigned long start_time = 0;
        if (!read_process_start_time(pid, &start_time)) {
            abend("process %d disappeared while recording working-set identity\n", pid);
        }
        result.emplace(pid, start_time);
    }
    return result;
}

std::map<pid_t, linux_procfs::SmapsDetail> read_tree_smaps_detail(
    const std::set<pid_t>& tree) {
    std::map<pid_t, linux_procfs::SmapsDetail> result;
    linux_procfs::SmapsReadBudget budget;
    for (pid_t pid : tree) {
        if (exiting != 0) throw CaptureInterrupted {};
        linux_procfs::SmapsDetail detail;
        std::string reason;
        if (!linux_procfs::read_smaps_detail(pid, &detail, &reason, &budget,
                                             []() { return exiting != 0; })) {
            if (exiting != 0) throw CaptureInterrupted {};
            abend("%s\n", reason.c_str());
        }
        result.emplace(pid, std::move(detail));
    }
    return result;
}

// Establish the start boundary while frozen, then let the exact application
// tree run for the requested interval. A second freeze and topology validation
// happen in collect_memftprt before any successful report is committed.
bool prepare_working_set(fp_prochandle* fp_proc,
                         std::optional<working_set_capture_state>* state_out) {
    state_out->reset();
    if (working_set_seconds == 0) return true;
    if (root_externally_stopped) {
        abend("--working-set cannot run an externally stopped target\n");
    }

    user_message("freezing process tree to begin a %u-second working-set window\n",
                 working_set_seconds);
    const std::set<pid_t> tree = freeze_process_tree(fp_proc->pid);
    working_set_capture_state state;
    state.freeze_backend = selected_freeze_backend;
    state.cgroup_path = selected_cgroup_path;
    state.process_identities = process_identities_for_tree(tree);
    {
        const std::map<pid_t, linux_procfs::SmapsDetail> baseline =
            read_tree_smaps_detail(tree);
        for (const auto& process : baseline) {
            state.mapping_identities[process.first] =
                linux_working_set::mapping_identities(process.second);
        }
    }
    for (pid_t pid : tree) {
        std::string reason;
        if (!linux_working_set::clear_referenced_bits(pid, &reason)) {
            abend("working-set setup failed for process %d: %s\n", pid, reason.c_str());
        }
    }

    // Auto mode is resolved once. Using the same backend at the end prevents
    // a method change from being mistaken for one consistent experiment.
    requested_freeze_mode = selected_freeze_backend == "cgroup" ? FreezeMode::cgroup
                                                                  : FreezeMode::signals;
    state.info.requested_window_seconds = working_set_seconds;
    state.info.started_timestamp_ns = system_timestamp_ns();
    state.started_monotonic = std::chrono::steady_clock::now();
    if (!thaw_cgroup()) {
        abend("could not thaw target after clearing referenced bits\n");
    }
    if (!resume_stopped_descendants()) {
        abend("could not resume descendants for the working-set window\n");
    }

    fp_proc->cursig = 0;
    continue_running(fp_proc);
    root_stopped_by_us = false;
    const WaitOutcome window_outcome = continue_until_elapsed(fp_proc, working_set_seconds);
    if (window_outcome == WaitOutcome::interrupted) return false;
    if (window_outcome == WaitOutcome::external_stop) {
        abend("--working-set window ended early because the target entered an external "
              "job-control stop\n");
    }
    *state_out = std::move(state);
    return true;
}

void write_capture_metadata(pid_t root_pid) {
    const std::string output_name =
        output_path("capture_metadata_" + std::to_string(root_pid) + ".tsv");
    std::string reason;
    std::ostringstream output;
    output << "key\tpid\tvalue\n";
    output << "freeze_backend\t" << root_pid << '\t' << selected_freeze_backend << '\n';
    output << "mapping_detail\t" << root_pid << '\t'
           << (mappings_enabled ? "captured" : "not_requested") << '\n';
    output << "reference_scope\t" << root_pid << '\t'
           << (captured_working_set.has_value() ? "since_clear_refs" : "kernel_current")
           << '\n';
    output << "pidfd\t" << root_pid << '\t' << pidfd_capture_status() << '\n';
    for (const auto& source : captured_smaps_sources) {
        output << "smaps_source\t" << source.first << '\t' << source.second << '\n';
    }
    for (const auto& snapshot : snapshot_capture_status) {
        output << "snapshot_" << snapshot.first << '\t' << root_pid << '\t'
               << snapshot.second << '\n';
    }
    if (captured_working_set.has_value()) {
        output << "working_set\t" << root_pid << "\tcaptured\n";
        output << "clear_refs\t" << root_pid << "\tused\n";
        output << "working_set_requested_seconds\t" << root_pid << '\t'
               << captured_working_set->requested_window_seconds << '\n';
        output << "working_set_actual_ns\t" << root_pid << '\t'
               << captured_working_set->actual_window_ns << '\n';
        output << "working_set_validation_scope\t" << root_pid << '\t'
               << captured_working_set->validation_scope << '\n';
        output << "working_set_process_identity_matches_at_boundaries\t" << root_pid
               << "\ttrue\n";
        output << "working_set_mapping_topology_matches_at_boundaries\t" << root_pid
               << "\ttrue\n";
    } else {
        output << "working_set\t" << root_pid << "\tnot_requested\n";
        output << "clear_refs\t" << root_pid << "\tnot_requested\n";
    }
    if (original_launcher_pid > 0 && original_launcher_pid != root_pid) {
        output << "launcher_pid\t" << root_pid << '\t' << original_launcher_pid << '\n';
    }
    if (!selected_cgroup_path.empty()) {
        output << "cgroup_path\t" << root_pid << '\t'
               << sanitize_tsv_field(selected_cgroup_path)
               << '\n';
    }
    for (const auto& capture : jvm_capture_status) {
        const runtime_capture_metadata& metadata = capture.second;
        output << "jvm_nmt\t" << capture.first << '\t' << metadata.status << '\n';
        output << "jvm_runtime\t" << capture.first << '\t' << metadata.runtime << '\n';
        if (!metadata.runtime_version.empty()) {
            output << "jvm_runtime_version\t" << capture.first << '\t'
                   << sanitize_tsv_field(metadata.runtime_version) << '\n';
        }
        if (metadata.timestamp_ns != 0) {
            output << "jvm_timestamp_ns\t" << capture.first << '\t' << metadata.timestamp_ns
                   << '\n';
        }
        output << "jvm_duration_ns\t" << capture.first << '\t' << metadata.duration_ns << '\n';
        output << "jvm_method\t" << capture.first << '\t'
               << sanitize_tsv_field(metadata.method) << '\n';
        output << "jvm_units\t" << capture.first << '\t' << metadata.units << '\n';
        output << "jvm_forced_gc\t" << capture.first << '\t'
               << (metadata.forced_gc ? "true" : "false") << '\n';
    }
    if (!write_atomic_text(output_name, output.str(), &reason)) {
        abend("%s\n", reason.c_str());
    }
}

void write_capture_complete(pid_t root_pid) {
    std::ostringstream output;
    output << "key\tpid\tvalue\n";
    output << "status\t" << root_pid << "\tcomplete\n";
    output << "artifact_count\t" << root_pid << '\t'
           << artifact_store->committed_count() << '\n';
    for (const std::string& artifact : artifact_store->committed_artifacts()) {
        output << "artifact\t" << root_pid << '\t'
               << sanitize_tsv_field(std::filesystem::path(artifact).filename().string()) << '\n';
    }

    const std::string output_name =
        output_path("capture_complete_" + std::to_string(root_pid) + ".tsv");
    std::string reason;
    if (!write_atomic_text(output_name, output.str(), &reason)) {
        abend("%s\n", reason.c_str());
    }
}

MemoryReport collect_memftprt(fp_prochandle* fp_proc,
                              working_set_capture_state* working_set_state = nullptr) {
    const std::set<pid_t> process_tree = freeze_process_tree(fp_proc->pid);
    // This is the end of the Referenced observation interval: after the freeze
    // returns, no member of the validated tree can acquire another accessed bit.
    const auto final_freeze_monotonic = std::chrono::steady_clock::now();
    const unsigned long long final_freeze_timestamp_ns = system_timestamp_ns();
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
    std::map<pid_t, linux_procfs::SmapsDetail> final_details;
    if (working_set_state != nullptr) {
        if (selected_freeze_backend != working_set_state->freeze_backend ||
            selected_cgroup_path != working_set_state->cgroup_path) {
            abend("working-set freeze backend or cgroup changed during the observation window\n");
        }
        const std::map<pid_t, unsigned long> final_process_identities =
            process_identities_for_tree(process_tree);
        if (final_process_identities != working_set_state->process_identities) {
            abend("working-set process identities did not match between boundary snapshots\n");
        }
        final_details = read_tree_smaps_detail(process_tree);
        for (const auto& process : final_details) {
            const auto baseline = working_set_state->mapping_identities.find(process.first);
            if (baseline == working_set_state->mapping_identities.end() ||
                baseline->second != linux_working_set::mapping_identities(process.second)) {
                abend("working-set mapping topology for process %d did not match between "
                      "boundary snapshots\n",
                      process.first);
            }
        }
        working_set_state->info.actual_window_ns = static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                final_freeze_monotonic - working_set_state->started_monotonic)
                .count());
        working_set_state->info.ended_timestamp_ns = final_freeze_timestamp_ns;
        working_set_state->info.process_identity_matches_at_boundaries = true;
        working_set_state->info.mapping_topology_matches_at_boundaries = true;
        captured_working_set = working_set_state->info;
    } else {
        captured_working_set.reset();
    }
    MemoryReport memory_report = build_process_tree_memory_report(
        fp_proc->pid, process_tree, final_freeze_timestamp_ns,
        final_details.empty() ? nullptr : &final_details);

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
    snapshot_capture_status["process_maps"] =
        run_snapshot_command("cat", {"/proc/" + pid + "/maps"}, maps_name)
            ? "captured"
            : "unavailable";
    snapshot_capture_status["process_ps"] =
        run_snapshot_command("ps",
                             {"-o", "pid,uid,state,rss,vsz,minflt,majflt,args", "-p", pid},
                             ps_name)
            ? "captured"
            : "unavailable";
    snapshot_capture_status["process_top"] =
        run_snapshot_command("top", {"-b", "-n", "1", "-p", pid}, top_name)
            ? "captured"
            : "unavailable";
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
    return memory_report;
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
    const WaitOutcome wait_outcome =
        wait_time == -1
            ? continue_until_quiescent(target_process)
            : continue_until_elapsed(target_process, static_cast<unsigned>(wait_time));
    if (wait_outcome == WaitOutcome::interrupted) {
        cleanup_capture_target();
        return;
    }
    if (abandon_interrupted_capture()) return;
    (void)capture_runtime_snapshots(target_process);
    if (abandon_interrupted_capture()) return;
    std::optional<working_set_capture_state> working_set_state;
    if (!prepare_working_set(target_process, &working_set_state)) {
        cleanup_capture_target();
        return;
    }
    user_message("freezing process tree and reading memory accounting\n");
    MemoryReport memory_report = collect_memftprt(
        target_process, working_set_state.has_value() ? &*working_set_state : nullptr);
    if (!quiet_mode) report();
    const pid_t captured_pid = target_process->pid;
    release_process(target_process);
    target_process = nullptr;
    if (exiting != 0) {
        release_output_lock();
        return;
    }
    write_memory_reports(memory_report);
    if (exiting != 0) {
        release_output_lock();
        return;
    }
    write_capture_metadata(captured_pid);
    if (exiting != 0) {
        release_output_lock();
        return;
    }
    write_capture_complete(captured_pid);
    if (exiting != 0) {
        std::string reason;
        const std::string marker =
            output_path("capture_complete_" + std::to_string(captured_pid) + ".tsv");
        if (!artifact_store->remove_file(marker, true, &reason)) abend("%s\n", reason.c_str());
        release_output_lock();
        return;
    }
    announce_completion(captured_pid);
    release_output_lock();
}

void analyze(char** argv) {
    initialize_library();
    target_process = start_process(argv);
    acquire_output_lock(target_process->pid);
    announce_wait(target_process->pid, "launched");
    const WaitOutcome wait_outcome =
        wait_time == -1
            ? continue_until_quiescent(target_process)
            : continue_until_elapsed(target_process, static_cast<unsigned>(wait_time));
    if (wait_outcome == WaitOutcome::interrupted) {
        cleanup_capture_target();
        return;
    }
    if (abandon_interrupted_capture()) return;
    (void)capture_runtime_snapshots(target_process);
    if (abandon_interrupted_capture()) return;
    std::optional<working_set_capture_state> working_set_state;
    if (!prepare_working_set(target_process, &working_set_state)) {
        cleanup_capture_target();
        return;
    }
    user_message("freezing process tree and reading memory accounting\n");
    MemoryReport memory_report = collect_memftprt(
        target_process, working_set_state.has_value() ? &*working_set_state : nullptr);
    if (!quiet_mode) report();
    const pid_t captured_pid = target_process->pid;
    if (kill_before_exit) {
        kill_process(target_process);
    } else {
        release_process(target_process);
    }
    target_process = nullptr;
    if (exiting != 0) {
        release_output_lock();
        return;
    }
    write_memory_reports(memory_report);
    if (exiting != 0) {
        release_output_lock();
        return;
    }
    write_capture_metadata(captured_pid);
    if (exiting != 0) {
        release_output_lock();
        return;
    }
    write_capture_complete(captured_pid);
    if (exiting != 0) {
        std::string reason;
        const std::string marker =
            output_path("capture_complete_" + std::to_string(captured_pid) + ".tsv");
        if (!artifact_store->remove_file(marker, true, &reason)) abend("%s\n", reason.c_str());
        release_output_lock();
        return;
    }
    announce_completion(captured_pid);
    release_output_lock();
}

}  // namespace

int run_capture(const CommandLine& command_line, char** argv) {
    const CaptureConfig& config = command_line.config;
    program_name = command_line.program_name.c_str();
    debug_level = config.debug_level;
    wait_time = config.wait_time;
    quiet_window_seconds = config.quiet_window_seconds;
    quiescence_timeout_seconds = config.quiescence_timeout_seconds;
    quiet_mode = config.quiet;
    requested_freeze_mode = config.freeze_mode;
    requested_report_format = config.report_format;
    jcmd_enabled = config.jcmd_enabled;
    enable_jvm_nmt = config.enable_jvm_nmt;
    mappings_enabled = config.mappings_enabled;
    working_set_seconds = config.working_set_seconds;
    requested_launch_cgroup = config.launch_cgroup;
    snapshot_capture_status.clear();
    artifact_store = std::make_unique<ArtifactStore>(config.output_directory);
    std::string output_reason;
    if (!artifact_store->prepare(&output_reason)) throw cli::Error(output_reason);

    try {
        if (command_line.target_pid != -1) {
            analyze_pid(static_cast<pid_t>(command_line.target_pid));
        } else {
            analyze(argv + command_line.command_index);
        }
    } catch (const CaptureInterrupted&) {
        cleanup_capture_target();
        return exiting == 0 ? 254 : 128 + static_cast<int>(exiting);
    } catch (const std::exception& error) {
        // In particular, std::bad_alloc can occur while assembling a detailed
        // mapping report. Never let a C++ exception bypass target cleanup.
        std::fprintf(stderr, "%s: aborting - unexpected failure: %s\n", program_name,
                     error.what());
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
        release_output_lock();
        return 254;
    } catch (...) {
        std::fprintf(stderr, "%s: aborting - unexpected non-standard failure\n",
                     program_name);
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
        release_output_lock();
        return 254;
    }
    return exiting == 0 ? 0 : 128 + static_cast<int>(exiting);
}

}  // namespace memftprt
