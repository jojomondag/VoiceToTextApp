#include "hyprland_ipc.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace vtt {
namespace hyprland {
namespace {

std::string socket_dir() {
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!signature || !*signature || !runtime || !*runtime) return {};
    return std::string(runtime) + "/hypr/" + signature;
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

} // namespace

bool available() {
    std::string dir = socket_dir();
    return !dir.empty() && access((dir + "/.socket.sock").c_str(), F_OK) == 0;
}

std::string event_socket_path() {
    std::string dir = socket_dir();
    return dir.empty() ? std::string() : dir + "/.socket2.sock";
}

bool request(const std::string& command, std::string& reply) {
    reply.clear();
    std::string dir = socket_dir();
    if (dir.empty()) return false;
    std::string path = dir + "/.socket.sock";

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    timeval timeout = {};
    timeout.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return false;
    }

    size_t sent = 0;
    while (sent < command.size()) {
        ssize_t n = write(fd, command.data() + sent, command.size() - sent);
        if (n <= 0) {
            close(fd);
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    char buffer[4096];
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        reply.append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    return true;
}

bool uses_lua() {
    // Decided once, but only after Hyprland actually answered.
    static std::mutex mutex;
    static bool known = false;
    static bool lua = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (!known) {
        std::string reply;
        if (request("eval return 1", reply)) {
            lua = trim(reply) == "ok";
            known = true;
        }
    }
    return lua;
}

std::string lua_quote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool parse_window(const nlohmann::json& json, Window& window) {
    if (!json.is_object()) return false;
    std::string address = json.value("address", std::string());
    if (address.empty()) return false;
    try {
        window.address = static_cast<std::uintptr_t>(std::stoull(address, nullptr, 16));
    } catch (const std::exception&) {
        return false;
    }
    window.pid = json.value("pid", 0L);
    window.window_class = lower(json.value("class", std::string()));
    return window.address != 0;
}

std::string address_selector(std::uintptr_t address) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "address:0x%llx",
                  static_cast<unsigned long long>(address));
    return buffer;
}

} // namespace

bool active_window(Window& window) {
    std::string reply;
    if (!request("j/activewindow", reply)) return false;
    try {
        return parse_window(nlohmann::json::parse(reply), window);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool find_window(std::uintptr_t address, Window& window) {
    std::string reply;
    if (!request("j/clients", reply)) return false;
    try {
        auto clients = nlohmann::json::parse(reply);
        if (!clients.is_array()) return false;
        for (const auto& client : clients) {
            Window candidate;
            if (parse_window(client, candidate) && candidate.address == address) {
                window = candidate;
                return true;
            }
        }
    } catch (const nlohmann::json::exception&) {
    }
    return false;
}

bool focus_window(std::uintptr_t address) {
    std::string selector = address_selector(address);
    std::string reply;
    std::string command = uses_lua()
        ? "dispatch hl.dsp.focus({ window = " + lua_quote(selector) + " })"
        : "dispatch focuswindow " + selector;
    return request(command, reply) && trim(reply) == "ok";
}

bool window_shown(long pid, const std::string& title_prefix, bool& shown) {
    std::string clients_reply;
    std::string monitors_reply;
    if (!request("j/clients", clients_reply) || !request("j/monitors", monitors_reply)) {
        return false;
    }
    try {
        auto clients = nlohmann::json::parse(clients_reply);
        auto monitors = nlohmann::json::parse(monitors_reply);
        if (!clients.is_array() || !monitors.is_array()) return false;

        // The ordinary workspace of every monitor plus an open special one.
        std::vector<long long> showing;
        for (const auto& monitor : monitors) {
            if (!monitor.is_object()) continue;
            for (const char* key : {"activeWorkspace", "specialWorkspace"}) {
                auto workspace = monitor.find(key);
                if (workspace == monitor.end() || !workspace->is_object()) continue;
                // No special workspace open is reported as id 0.
                const long long id = workspace->value("id", 0LL);
                if (id != 0) showing.push_back(id);
            }
        }

        bool found = false;
        for (const auto& client : clients) {
            if (!client.is_object() || client.value("pid", 0L) != pid) continue;
            if (client.value("title", std::string()).rfind(title_prefix, 0) != 0) continue;
            if (!client.value("mapped", true) || client.value("hidden", false)) continue;
            auto workspace = client.find("workspace");
            if (workspace == client.end() || !workspace->is_object()) continue;
            const long long id = workspace->value("id", 0LL);
            if (std::find(showing.begin(), showing.end(), id) != showing.end()) {
                found = true;
                break;
            }
        }
        shown = found;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool focus_previous_window(long pid, const std::string& title_prefix) {
    std::string reply;
    if (!request("j/clients", reply)) return false;
    std::uintptr_t previous = 0;
    try {
        auto clients = nlohmann::json::parse(reply);
        if (!clients.is_array()) return false;

        // focusHistoryID: 0 is the focused window, 1 the one before it, ...
        bool focused = false;
        long long best = 0;
        for (const auto& client : clients) {
            if (!client.is_object()) continue;
            if (!client.value("mapped", true) || client.value("hidden", false)) continue;
            const long long order = client.value("focusHistoryID", -1LL);
            const bool own = client.value("pid", 0L) == pid &&
                client.value("title", std::string()).rfind(title_prefix, 0) == 0;
            if (own) {
                if (order == 0) focused = true;
                continue;
            }
            Window candidate;
            if (order < 1 || (previous && order >= best)) continue;
            if (!parse_window(client, candidate)) continue;
            previous = candidate.address;
            best = order;
        }
        if (!focused || !previous) return false;
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    if (uses_lua()) {
        // One request, so the pointer is back before a frame shows it moved.
        const std::string focus =
            "hl.dsp.focus({ window = " + lua_quote(address_selector(previous)) + " })";
        if (request("eval local p = hl.get_cursor_pos(); hl.dispatch(" + focus +
                    "); if p then hl.dispatch(hl.dsp.cursor.move({ x = p.x, y = p.y })) end",
                    reply) &&
            trim(reply) == "ok") {
            return true;
        }
    }
    return focus_window(previous);
}

} // namespace hyprland
} // namespace vtt
