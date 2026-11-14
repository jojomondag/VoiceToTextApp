#include "wayland_input.h"

#include "log.h"
#include "virtual_keyboard.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace vtt {
namespace wayland_input {

bool run(const std::vector<std::string>& arguments, const std::string* input,
         std::string* output) {
    if (arguments.empty()) return false;

    // A write to a tool that has already exited must not kill the app.
    static std::once_flag once;
    std::call_once(once, []() { std::signal(SIGPIPE, SIG_IGN); });

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (input && pipe2(in_pipe, O_CLOEXEC) != 0) return false;
    if (output && pipe2(out_pipe, O_CLOEXEC) != 0) {
        if (input) { close(in_pipe[0]); close(in_pipe[1]); }
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (input) {
        posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    if (output) {
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    } else {
        // wl-copy keeps serving the clipboard in the background; it must not
        // hold on to our output streams.
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawn_result = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (input) close(in_pipe[0]);
    if (output) close(out_pipe[1]);
    if (spawn_result != 0) {
        if (input) close(in_pipe[1]);
        if (output) close(out_pipe[0]);
        vtlog("input: cannot run %s (%d)", arguments[0].c_str(), spawn_result);
        return false;
    }

    if (input) {
        size_t written = 0;
        while (written < input->size()) {
            const ssize_t n = write(in_pipe[1], input->data() + written, input->size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
        close(in_pipe[1]);
    }

    if (output) {
        output->clear();
        char buffer[4096];
        for (;;) {
            const ssize_t n = read(out_pipe[0], buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            output->append(buffer, static_cast<size_t>(n));
        }
        close(out_pipe[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool can_type(const std::string& text) {
    return virtual_keyboard::can_type(text);
}

bool type_text(const std::string& text, int delay_ms) {
    return virtual_keyboard::type_text(text, delay_ms);
}

bool press_key(const std::string& key, const std::vector<std::string>& modifiers, int count) {
    return virtual_keyboard::press_key(key, modifiers, count);
}

bool set_clipboard(const std::string& text) {
    // --sensitive asks clipboard managers not to keep the dictated text.
    return run({"wl-copy", "--sensitive", "--type", "text/plain;charset=utf-8"}, &text);
}

bool clear_clipboard() {
    return run({"wl-copy", "--clear"});
}

bool get_clipboard(std::string& text) {
    text.clear();
    return run({"wl-paste", "--no-newline", "--type", "text/plain;charset=utf-8"}, nullptr, &text) ||
           run({"wl-paste", "--no-newline", "--type", "text/plain"}, nullptr, &text);
}

bool is_terminal_class(const std::string& window_class) {
    static const char* const terminals[] = {
        "kitty", "alacritty", "ghostty", "foot", "wezterm", "konsole",
        "gnome-terminal", "ptyxis", "kgx", "tilix", "terminator", "xterm",
        "st-256color", "rio", "warp", "contour", "cool-retro-term",
    };
    for (const char* terminal : terminals) {
        if (window_class.find(terminal) != std::string::npos) return true;
    }
    return false;
}

bool send_paste(bool terminal) {
    return terminal ? press_key("v", {"ctrl", "shift"}) : press_key("v", {"ctrl"});
}

} // namespace wayland_input
} // namespace vtt
