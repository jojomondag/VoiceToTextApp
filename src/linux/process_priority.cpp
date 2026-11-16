#include "process_priority.h"

#include "log.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <thread>

extern char** environ;

namespace vtt {
namespace {

// Weight against the default of 100 that every other app unit has. Any
// CPUWeight makes systemd enable the cpu controller for the app slice, so
// each app unit becomes its own scheduling group; the weight then decides
// the share when they compete. Measured on 24 cores with 96 busy loops in a
// sibling scope, on top of another ~20-core job: Stop drained in 0.26-0.70 s
// instead of 1.56-3.43 s, with 23-28 live previews per 10 s of speech
// instead of 6-10. Weight 100 gave 0.89-1.01 s and 14-15 previews.
constexpr const char* kCpuWeight = "CPUWeight=1000";

// The unit this process runs in, e.g. "app-Hyprland-VoiceToText-1a2b.scope",
// or empty when it is not a unit of the user's own systemd manager or when
// other processes share it (started from a terminal, the unit is the
// terminal's, and raising it would raise everything started there).
std::string own_user_unit() {
    std::ifstream cgroup_file("/proc/self/cgroup");
    std::string line, path;
    while (std::getline(cgroup_file, line)) {
        if (line.rfind("0::", 0) == 0) path = line.substr(3);
    }
    const std::string manager = "/user@" + std::to_string(getuid()) + ".service/";
    if (path.find(manager) == std::string::npos) return {};

    const std::string unit = path.substr(path.find_last_of('/') + 1);
    const auto ends_with = [&unit](const std::string& suffix) {
        return unit.size() > suffix.size() &&
               unit.compare(unit.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (!ends_with(".scope") && !ends_with(".service")) return {};

    std::ifstream procs("/sys/fs/cgroup" + path + "/cgroup.procs");
    const std::string self = std::to_string(getpid());
    bool found_self = false;
    while (std::getline(procs, line)) {
        if (line != self) return {};
        found_self = true;
    }
    return found_self ? unit : std::string();
}

} // namespace

void raise_own_cpu_weight() {
    const std::string unit = own_user_unit();
    if (unit.empty()) {
        vtlog("cpu weight: not raised (not alone in a user unit)");
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char* const argv[] = {
        const_cast<char*>("systemctl"), const_cast<char*>("--user"),
        const_cast<char*>("set-property"), const_cast<char*>("--runtime"),
        const_cast<char*>(unit.c_str()), const_cast<char*>(kCpuWeight), nullptr};
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, "systemctl", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        vtlog("cpu weight: systemctl could not be started (%d)", spawned);
        return;
    }

    // Startup does not wait for the user manager.
    std::thread([pid, unit]() {
        int status = 0;
        const bool ok = waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                        WEXITSTATUS(status) == 0;
        vtlog("cpu weight: %s on %s %s", kCpuWeight, unit.c_str(), ok ? "set" : "failed");
    }).detach();
}

} // namespace vtt
