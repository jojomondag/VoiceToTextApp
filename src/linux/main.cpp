#include "app.h"
#include "app_icon.h"

#include <QApplication>
#include <QMessageBox>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// One instance per login session. It listens on this socket; a second
// VoiceToText process either hands it a command (--toggle, ...) or, started
// without one, just exits - the counterpart of the named mutex on Windows.
std::string socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = runtime && *runtime ? runtime : "/tmp";
    return dir + "/voicetotext-" + std::to_string(getuid()) + ".sock";
}

bool fill_address(sockaddr_un& address) {
    std::string path = socket_path();
    address = {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Sends a command to the running instance. False when there is none.
bool send_command(const std::string& command) {
    sockaddr_un address;
    if (!fill_address(address)) return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    timeval timeout = {};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    bool ok = false;
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        std::string line = command + "\n";
        ok = write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    }
    close(fd);
    return ok;
}

int open_server_socket() {
    sockaddr_un address;
    if (!fill_address(address)) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    unlink(address.sun_path); // stale socket of an instance that is gone
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Name of a process and its parents ("VoiceToText < sh < Hyprland"), for the
// log: it tells which key bind, bar button or script sent a command.
std::string process_chain(pid_t pid) {
    std::string chain;
    for (int depth = 0; depth < 4 && pid > 1; ++depth) {
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (!std::getline(stat, line)) break;
        const auto open = line.find('(');
        const auto close = line.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) break;
        if (!chain.empty()) chain += " < ";
        chain += line.substr(open + 1, close - open - 1);
        std::istringstream rest(line.substr(close + 2));
        std::string state;
        long parent = 0;
        rest >> state >> parent;
        pid = static_cast<pid_t>(parent);
    }
    return chain;
}

// Reads the command of one client: a single short line. `source` names the
// sending process and its parents.
std::string read_command(int server_fd, std::string& source) {
    source.clear();
    int client = accept4(server_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) return {};

    ucred credentials = {};
    socklen_t length = sizeof(credentials);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0) {
        source = process_chain(credentials.pid);
    }

    timeval timeout = {};
    timeout.tv_usec = 200 * 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string command;
    char buffer[64];
    while (command.size() < 64) {
        ssize_t n = read(client, buffer, sizeof(buffer));
        if (n <= 0) break;
        command.append(buffer, static_cast<size_t>(n));
        if (command.find('\n') != std::string::npos) break;
    }
    close(client);

    size_t end = command.find_first_of("\r\n");
    if (end != std::string::npos) command.resize(end);
    return command;
}

int g_signal_pipe[2] = {-1, -1};

void on_terminate_signal(int) {
    char byte = 1;
    ssize_t ignored = write(g_signal_pipe[1], &byte, 1);
    (void)ignored;
}

void print_usage() {
    std::puts(
        "VoiceToText - local dictation\n"
        "\n"
        "Usage: VoiceToText [command]\n"
        "\n"
        "Without a command the app starts in the system tray. With one, it is\n"
        "passed to the instance that is already running:\n"
        "  --toggle     start or stop recording\n"
        "  --cancel     cancel the current recording\n"
        "  --mode-recorded | --mode-clean | --mode-ai\n"
        "               switch the live mode while recording\n"
        "               (Rösttext, Städa text, AI-språk)\n"
        "  --dictation  open the live dictation window\n"
        "  --history    open the History window\n"
        "  --settings   open config.json\n"
        "  --quit       exit the running instance");
}

} // namespace

int main(int argc, char** argv) {
    std::string command;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--toggle" || arg == "--cancel" || arg == "--language" ||
            arg == "--history" || arg == "--settings" || arg == "--quit" ||
            arg == "--dictation" || arg == "--mode-recorded" ||
            arg == "--mode-clean" || arg == "--mode-ai") {
            command = arg.substr(2);
        }
    }

    if (!command.empty()) {
        if (send_command(command)) return 0;
        std::fprintf(stderr, "VoiceToText is not running.\n");
        return 1;
    }

    // Single-instance guard: another process already owns the socket, so
    // terminate silently.
    if (send_command("ping")) return 0;

    int server_fd = open_server_socket();

    QApplication qt_app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VoiceToText"));
    QApplication::setApplicationDisplayName(QStringLiteral("VoiceToText"));
    QApplication::setDesktopFileName(QStringLiteral("voicetotext"));
    QApplication::setWindowIcon(vtt::create_app_icon(QColor(0, 120, 215)));
    // The app lives in the tray; closing the History window must not end it.
    QApplication::setQuitOnLastWindowClosed(false);

    int result = 1;
    {
        vtt::App app;

        QSocketNotifier* commands = nullptr;
        if (server_fd >= 0) {
            commands = new QSocketNotifier(server_fd, QSocketNotifier::Read, &qt_app);
            QObject::connect(commands, &QSocketNotifier::activated, &qt_app,
                             [&app, server_fd]() {
                std::string source;
                std::string received = read_command(server_fd, source);
                if (!received.empty() && received != "ping") {
                    app.handle_command(received, source);
                }
            });
        }

        // SIGINT/SIGTERM leave through the normal shutdown path, so the
        // compositor key binds are removed again.
        if (pipe2(g_signal_pipe, O_CLOEXEC | O_NONBLOCK) == 0) {
            auto* terminate = new QSocketNotifier(g_signal_pipe[0],
                                                QSocketNotifier::Read, &qt_app);
            QObject::connect(terminate, &QSocketNotifier::activated, &qt_app, []() {
                char buffer[16];
                while (read(g_signal_pipe[0], buffer, sizeof(buffer)) > 0) {}
                QCoreApplication::quit();
            });
            std::signal(SIGINT, on_terminate_signal);
            std::signal(SIGTERM, on_terminate_signal);
        }

        if (!app.initialize(nullptr)) {
            QMessageBox::critical(
                nullptr, QStringLiteral("VoiceToText - Error"),
                QStringLiteral("Failed to initialize VoiceToText.\n\n"
                               "Please ensure:\n"
                               "  - A microphone is connected\n"
                               "  - The speech runtime libraries are next to the program"));
        } else {
            result = app.run();
        }
        app.shutdown();
    }

    if (server_fd >= 0) {
        close(server_fd);
        unlink(socket_path().c_str());
    }
    return result;
}
