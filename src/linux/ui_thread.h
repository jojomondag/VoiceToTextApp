#pragma once
// Runs a callable on the Qt UI thread. The application core calls into the
// tray, overlay and history window from worker threads (Win32 tolerates
// that); Qt widgets may only be touched from the thread that owns them.

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <utility>

namespace vtt {
namespace ui {

template <typename Function>
void run(Function&& function) {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return;

    if (QThread::currentThread() == app->thread()) {
        function();
    } else {
        QMetaObject::invokeMethod(app, std::forward<Function>(function),
                                  Qt::QueuedConnection);
    }
}

} // namespace ui
} // namespace vtt
