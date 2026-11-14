#include "win_compat.h"

#include "hyprland_ipc.h"
#include "log.h"

#include <QApplication>
#include <QMessageBox>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace vtt {
namespace compat {
namespace {

struct MessageWindow {
    WindowProc proc = nullptr;
    void* user_data = nullptr;
    std::map<UINT_PTR, QTimer*> timers; // UI thread only
};

// Windows are looked up by handle so that a message posted to a window that is
// already gone is dropped instead of touching freed memory.
std::mutex g_windows_mutex;
std::map<HWND, std::shared_ptr<MessageWindow>> g_windows;

std::shared_ptr<MessageWindow> find_window(HWND window) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(window);
    return it == g_windows.end() ? nullptr : it->second;
}

void dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto target = find_window(window);
    if (!target || !target->proc) return;
    // DefWindowProc turns WM_CLOSE into the destruction of the window.
    if (message == WM_CLOSE) message = WM_DESTROY;
    target->proc(window, message, wparam, lparam);
}

} // namespace

HWND create_message_window(WindowProc proc, void* user_data) {
    auto window = std::make_shared<MessageWindow>();
    window->proc = proc;
    window->user_data = user_data;
    HWND handle = window.get();
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    g_windows[handle] = std::move(window);
    return handle;
}

void destroy_message_window(HWND window) {
    std::shared_ptr<MessageWindow> target;
    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        auto it = g_windows.find(window);
        if (it == g_windows.end()) return;
        target = std::move(it->second);
        g_windows.erase(it);
    }
    for (auto& [id, timer] : target->timers) delete timer;
    target->timers.clear();
}

void* window_user_data(HWND window) {
    auto target = find_window(window);
    return target ? target->user_data : nullptr;
}

} // namespace compat
} // namespace vtt

BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || !vtt::compat::find_window(window)) return FALSE;
    QMetaObject::invokeMethod(app, [window, message, wparam, lparam]() {
        vtt::compat::dispatch(window, message, wparam, lparam);
    }, Qt::QueuedConnection);
    return TRUE;
}

void PostQuitMessage(int exit_code) {
    QCoreApplication::exit(exit_code);
}

UINT_PTR SetTimer(HWND window, UINT_PTR id, UINT milliseconds, void*) {
    auto target = vtt::compat::find_window(window);
    if (!target) return 0;

    QTimer*& timer = target->timers[id];
    if (!timer) {
        timer = new QTimer();
        QObject::connect(timer, &QTimer::timeout, timer, [window, id]() {
            vtt::compat::dispatch(window, WM_TIMER, id, 0);
        });
    }
    timer->start(static_cast<int>(milliseconds));
    return id;
}

BOOL KillTimer(HWND window, UINT_PTR id) {
    auto target = vtt::compat::find_window(window);
    if (!target) return FALSE;
    auto it = target->timers.find(id);
    if (it == target->timers.end()) return FALSE;
    it->second->stop();
    it->second->deleteLater();
    target->timers.erase(it);
    return TRUE;
}

ULONGLONG GetTickCount64() {
    return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Sleep(DWORD milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

DWORD GetCurrentProcessId() {
    return static_cast<DWORD>(getpid());
}

int MessageBoxW(HWND, const wchar_t* text, const wchar_t* caption, UINT) {
    QString message = QString::fromWCharArray(text ? text : L"");
    vtt::vtlog("error: %s", message.toUtf8().constData());
    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app && QThread::currentThread() == app->thread()) {
        QMessageBox::warning(nullptr, QString::fromWCharArray(caption ? caption : L"VoiceToText"),
                             message);
    }
    return 1;
}

HWND GetForegroundWindow() {
    vtt::hyprland::Window window;
    if (!vtt::hyprland::active_window(window)) return nullptr;
    return reinterpret_cast<HWND>(window.address);
}

BOOL IsWindow(HWND window) {
    if (!window) return FALSE;
    vtt::hyprland::Window found;
    return vtt::hyprland::find_window(reinterpret_cast<std::uintptr_t>(window), found) ? TRUE : FALSE;
}

DWORD GetWindowThreadProcessId(HWND window, DWORD* process_id) {
    if (process_id) *process_id = 0;
    if (!window) return 0;
    vtt::hyprland::Window found;
    if (!vtt::hyprland::find_window(reinterpret_cast<std::uintptr_t>(window), found)) return 0;
    if (process_id) *process_id = static_cast<DWORD>(found.pid);
    return static_cast<DWORD>(found.pid);
}

BOOL SetForegroundWindow(HWND window) {
    if (!window) return FALSE;
    return vtt::hyprland::focus_window(reinterpret_cast<std::uintptr_t>(window)) ? TRUE : FALSE;
}
