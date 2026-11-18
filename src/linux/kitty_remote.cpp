#include "kitty_remote.h"

#include "log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace vtt {
namespace kitty_remote {
namespace {

const char kPrefix[] = "\x1bP@kitty-cmd";
const char kSuffix[] = "\x1b\\";
// Any version older than kitty's own is accepted; this one already had all the
// commands used here.
const nlohmann::json kVersion = {0, 14, 2};
// Keys per command: kitty sends them as one press, then repeats, then the
// releases, so very long lists are split.
constexpr int kKeysPerCommand = 200;
// Bytes of text per command, cut at a character boundary.
constexpr std::size_t kTextPerCommand = 2048;

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string process_name(long pid) {
    std::string name = read_file("/proc/" + std::to_string(pid) + "/comm");
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}

long parent_of(long pid) {
    // "pid (comm) state ppid ...": comm may itself hold spaces and parentheses.
    const std::string stat = read_file("/proc/" + std::to_string(pid) + "/stat");
    const auto close = stat.rfind(')');
    if (close == std::string::npos || close + 2 >= stat.size()) return 0;
    std::istringstream in(stat.substr(close + 2));
    char state = 0;
    long ppid = 0;
    in >> state >> ppid;
    return ppid;
}

std::vector<long> children_of(long pid) {
    std::vector<long> children;
    DIR* dir = opendir("/proc");
    if (!dir) return children;
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long candidate = std::strtol(entry->d_name, &end, 10);
        if (candidate <= 0 || !end || *end) continue;
        if (parent_of(candidate) == pid) children.push_back(candidate);
    }
    closedir(dir);
    return children;
}

// One variable from a process's environment (as it was when it started).
std::string environment_value(long pid, const std::string& name) {
    const std::string environ = read_file("/proc/" + std::to_string(pid) + "/environ");
    const std::string key = name + "=";
    for (std::size_t begin = 0; begin < environ.size();) {
        std::size_t end = environ.find('\0', begin);
        if (end == std::string::npos) end = environ.size();
        if (environ.compare(begin, key.size(), key) == 0) {
            return environ.substr(begin + key.size(), end - begin - key.size());
        }
        begin = end + 1;
    }
    return {};
}

// Sends one command and waits for kitty's answer, so commands run in the
// order they were sent. `answer` receives the reply's "data" when wanted.
bool exchange(const std::string& socket_path, const std::string& name,
              const nlohmann::json& payload, nlohmann::json* answer = nullptr) {
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (socket_path.empty() || socket_path.size() >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    timeval timeout = {};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return false;
    }

    const nlohmann::json command = {
        {"cmd", name}, {"version", kVersion}, {"no_response", false}, {"payload", payload}};
    // ASCII only (\u escapes), as kitty's own client sends it.
    const std::string message = std::string(kPrefix) +
        command.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace) + kSuffix;
    for (std::size_t sent = 0; sent < message.size();) {
        const ssize_t n = write(fd, message.data() + sent, message.size() - sent);
        if (n <= 0) {
            close(fd);
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }

    std::string reply;
    char buffer[8192];
    while (reply.size() < 2 || reply.compare(reply.size() - 2, 2, kSuffix) != 0) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        reply.append(buffer, static_cast<std::size_t>(n));
    }
    close(fd);

    const std::size_t prefix = std::strlen(kPrefix);
    if (reply.size() < prefix + 2 || reply.compare(0, prefix, kPrefix) != 0) {
        vtlog("kitty: no answer to %s", name.c_str());
        return false;
    }
    try {
        const auto parsed = nlohmann::json::parse(reply.substr(prefix, reply.size() - prefix - 2));
        if (!parsed.value("ok", false)) {
            vtlog("kitty: %s refused: %s", name.c_str(), parsed.value("error", std::string()).c_str());
            return false;
        }
        if (answer) *answer = parsed.contains("data") ? parsed["data"] : nlohmann::json();
        return true;
    } catch (const nlohmann::json::exception&) {
        vtlog("kitty: unreadable answer to %s", name.c_str());
        return false;
    }
}

bool flag(const nlohmann::json& object, const char* name) {
    return object.is_object() && object.value(name, false);
}

// The focused entry of a list (or the active one, or the first).
const nlohmann::json* pick(const nlohmann::json& list) {
    if (!list.is_array() || list.empty()) return nullptr;
    for (const auto& item : list) if (flag(item, "is_focused")) return &item;
    for (const auto& item : list) if (flag(item, "is_active")) return &item;
    return &list.front();
}

// The window active in kitty now, and the pid of the program it runs.
const nlohmann::json* active_window(const nlohmann::json& os_windows) {
    const auto* os_window = pick(os_windows);
    if (!os_window || !os_window->contains("tabs")) return nullptr;
    const auto* tab = pick((*os_window)["tabs"]);
    if (!tab || !tab->contains("windows")) return nullptr;
    return pick((*tab)["windows"]);
}

} // namespace

bool find(long pid, Target& target) {
    if (pid <= 0 || process_name(pid) != "kitty") return false;

    // Only the programs kitty runs in its windows say where its socket is.
    // Its helper processes (kitten __atexit__, __watch_conf__) carry the
    // environment kitty itself was started with, which names the socket of
    // whatever kitty launched it: they are told apart by KITTY_PID.
    const std::string own_pid = std::to_string(pid);
    const std::vector<long> children = children_of(pid);
    std::string socket_path;
    for (const long child : children) {
        if (environment_value(child, "KITTY_PID") != own_pid) continue;
        const std::string listen = environment_value(child, "KITTY_LISTEN_ON");
        // Only a socket file: an abstract socket (unix:@...) has no
        // permissions, any local user could write to it.
        if (listen.rfind("unix:/", 0) == 0) {
            socket_path = listen.substr(5);
            break;
        }
    }
    if (socket_path.empty()) return false;

    // The window to write to, proven to belong to this kitty: the program in
    // it must be one of this process's children.
    nlohmann::json listing;
    if (!exchange(socket_path, "ls", nlohmann::json::object(), &listing) || !listing.is_string()) {
        vtlog("kitty: pid %ld did not list its windows", pid);
        return false;
    }
    int window_id = 0;
    long window_pid = 0;
    try {
        // Kept in a variable: active_window() points into it.
        const nlohmann::json os_windows = nlohmann::json::parse(listing.get<std::string>());
        if (const auto* window = active_window(os_windows)) {
            window_id = window->value("id", 0);
            window_pid = window->value("pid", 0L);
        }
    } catch (const nlohmann::json::exception&) {
    }
    if (!window_id || std::find(children.begin(), children.end(), window_pid) == children.end()) {
        vtlog("kitty: the socket of pid %ld did not answer for its own window; not used", pid);
        return false;
    }
    target.socket = socket_path;
    target.window_id = window_id;
    return true;
}

bool send_text(const Target& target, const std::string& utf8) {
    if (!target.valid()) return false;
    const std::string match = "id:" + std::to_string(target.window_id);
    for (std::size_t begin = 0; begin < utf8.size();) {
        std::size_t end = (std::min)(utf8.size(), begin + kTextPerCommand);
        // Never split a character: back up to the start of a UTF-8 sequence.
        while (end < utf8.size() && end > begin &&
               (static_cast<unsigned char>(utf8[end]) & 0xC0) == 0x80) {
            --end;
        }
        if (end == begin) end = (std::min)(utf8.size(), begin + kTextPerCommand);
        if (!exchange(target.socket, "send-text",
                      {{"match", match}, {"data", "text:" + utf8.substr(begin, end - begin)}})) {
            return false;
        }
        begin = end;
    }
    return true;
}

bool send_key(const Target& target, const std::string& key, int count) {
    if (!target.valid()) return false;
    const std::string match = "id:" + std::to_string(target.window_id);
    while (count > 0) {
        const int batch = (std::min)(count, kKeysPerCommand);
        if (!exchange(target.socket, "send-key",
                      {{"match", match}, {"keys", std::vector<std::string>(
                                                     static_cast<std::size_t>(batch), key)}})) {
            return false;
        }
        count -= batch;
    }
    return true;
}

} // namespace kitty_remote
} // namespace vtt
