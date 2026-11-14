#include "tray_icon.h"

#include "app_icon.h"
#include "ui_thread.h"

#include <QAction>
#include <QMenu>
#include <QSystemTrayIcon>

#include <algorithm>

namespace vtt {

// Lives on the UI thread. Kept alive by the closures queued to that thread,
// so a late call from a worker thread never touches a destroyed tray.
struct TrayIcon::State {
    QSystemTrayIcon* tray = nullptr;
    QMenu* menu = nullptr;
    QIcon icon_idle;
    QIcon icon_recording;
    TrayCallback callback;
    bool recording = false;
    std::string current_model_size = "onnx-int8";
    std::string current_whisper_compute_mode = "auto";
    std::string current_gemma_compute_mode = "auto";
    std::string current_gemma_model = "auto";
    std::string recommended_gemma_model = "e2b";
    std::vector<std::string> installed_gemma_models;

    void emit_action(TrayAction action, const std::string& value = {}) {
        // Copy: the callback may tear the tray down (Quit).
        TrayCallback target = callback;
        if (target) target(action, value);
    }

    QAction* add(QMenu* parent, const QString& label, TrayAction action,
                 const std::string& value = {}) {
        QAction* item = parent->addAction(label);
        QObject::connect(item, &QAction::triggered, item, [this, action, value]() {
            emit_action(action, value);
        });
        return item;
    }

    QAction* add_checked(QMenu* parent, const QString& label, TrayAction action,
                         const std::string& value, bool checked) {
        QAction* item = add(parent, label, action, value);
        item->setCheckable(true);
        item->setChecked(checked);
        return item;
    }

    void rebuild_menu() {
        if (!menu) return;
        menu->clear();

        add(menu, recording ? QStringLiteral("Stop Recording")
                            : QStringLiteral("Start Recording"),
            TrayAction::ToggleRecording);

        menu->addSeparator();
        add(menu, QStringLiteral("Live dictation..."), TrayAction::OpenDictation);
        menu->addAction(QStringLiteral("Speech language: Swedish (Pianissimo)"))
            ->setEnabled(false);

        // Compute Mode - per-component submenus
        QMenu* compute_menu = menu->addMenu(QStringLiteral("Compute Mode"));
        QMenu* whisper_compute = compute_menu->addMenu(QStringLiteral("Pianissimo"));
        const bool onnx = current_model_size.rfind("onnx-", 0) == 0;
        add_checked(whisper_compute, QStringLiteral("GPU (Vulkan, GGUF)"),
                    TrayAction::SelectWhisperComputeMode, "gpu",
                    current_whisper_compute_mode == "gpu")->setEnabled(!onnx);
        add_checked(whisper_compute, QStringLiteral("CPU (GGUF / ONNX)"),
                    TrayAction::SelectWhisperComputeMode, "cpu",
                    current_whisper_compute_mode == "cpu");
        QMenu* gemma_compute = compute_menu->addMenu(QStringLiteral("Gemma"));
        add_checked(gemma_compute, QStringLiteral("GPU"),
                    TrayAction::SelectGemmaComputeMode, "gpu",
                    current_gemma_compute_mode == "gpu");
        add_checked(gemma_compute, QStringLiteral("CPU"),
                    TrayAction::SelectGemmaComputeMode, "cpu",
                    current_gemma_compute_mode == "cpu");
        compute_menu->addSeparator();
        compute_menu->addAction(QStringLiteral("Pianissimo: ") +
            QString::fromStdString(current_whisper_compute_mode))->setEnabled(false);
        compute_menu->addAction(QStringLiteral("Gemma: ") +
            QString::fromStdString(current_gemma_compute_mode))->setEnabled(false);

        QMenu* model_menu = menu->addMenu(QStringLiteral("Pianissimo Model"));
        struct ModelItem {
            const char* label;
            const char* id;
        };
        const ModelItem models[] = {
            {"GGUF Q8 (~740 MB)", "gguf-q8"},
            {"ONNX int4 - smallest (~515 MB)", "onnx-int4"},
            {"ONNX int8 - recommended (~660 MB)", "onnx-int8"},
            {"ONNX fp16 (~1.28 GB)", "onnx-fp16"},
            {"ONNX fp32 (~2.56 GB)", "onnx-fp32"},
        };
        for (const auto& model : models) {
            add_checked(model_menu, QString::fromUtf8(model.label),
                        TrayAction::SelectModel, model.id,
                        current_model_size == model.id);
        }

        QMenu* gemma_menu = menu->addMenu(QStringLiteral("Gemma Model"));
        struct GemmaItem {
            QString label;
            const char* model;
        };
        const GemmaItem gemma_models[] = {
            {QStringLiteral("Auto - recommends ") +
                 QString::fromStdString(recommended_gemma_model), "auto"},
            {QStringLiteral("Gemma 4 E2B - recommended"), "e2b"},
            {QStringLiteral("Gemma 4 E4B - higher quality"), "e4b"},
            {QStringLiteral("Gemma 4 12B - large"), "12b"},
            {QStringLiteral("Gemma 4 26B A4B - very large"), "26b-a4b"},
            {QStringLiteral("Gemma 4 31B - overkill"), "31b"},
        };
        for (const auto& model : gemma_models) {
            QString label = model.label;
            const bool installed =
                std::find(installed_gemma_models.begin(), installed_gemma_models.end(),
                          model.model) != installed_gemma_models.end();
            if (installed) label += QStringLiteral(" - installed");
            add_checked(gemma_menu, label, TrayAction::SelectGemmaModel, model.model,
                        current_gemma_model == model.model);
        }

        add(menu, QStringLiteral("History..."), TrayAction::OpenHistory);
        add(menu, QStringLiteral("Settings..."), TrayAction::OpenSettings);
        menu->addSeparator();
        add(menu, QStringLiteral("Quit"), TrayAction::Quit);
    }

    void destroy() {
        if (tray) {
            tray->hide();
            delete tray;
            tray = nullptr;
        }
        delete menu;
        menu = nullptr;
    }
};

TrayIcon::TrayIcon() : state_(std::make_shared<State>()) {}

TrayIcon::~TrayIcon() {
    shutdown();
}

bool TrayIcon::initialize(HWND /*hwnd*/, HINSTANCE /*hInstance*/) {
    State* state = state_.get();
    if (state->tray) return true;

    // Blue for idle, red for recording.
    state->icon_idle = create_app_icon(QColor(0, 120, 215));
    state->icon_recording = create_app_icon(QColor(220, 50, 50));

    state->menu = new QMenu();
    state->rebuild_menu();

    state->tray = new QSystemTrayIcon(state->icon_idle);
    state->tray->setToolTip(QStringLiteral("VoiceToText - Ready"));
    state->tray->setContextMenu(state->menu);
    QObject::connect(state->tray, &QSystemTrayIcon::activated, state->tray,
                     [state](QSystemTrayIcon::ActivationReason reason) {
        // A click on the icon starts/stops recording (double-click on Windows).
        if (reason == QSystemTrayIcon::Trigger ||
            reason == QSystemTrayIcon::DoubleClick) {
            state->emit_action(TrayAction::ToggleRecording);
        }
    });
    // Shown as soon as a tray host is around; none is not an error here.
    state->tray->show();
    return true;
}

void TrayIcon::shutdown() {
    ui::run([state = state_]() { state->destroy(); });
}

void TrayIcon::set_callback(TrayCallback callback) {
    ui::run([state = state_, callback = std::move(callback)]() {
        state->callback = callback;
    });
}

void TrayIcon::set_recording(bool recording) {
    ui::run([state = state_, recording]() {
        state->recording = recording;
        if (!state->tray) return;
        state->tray->setIcon(recording ? state->icon_recording : state->icon_idle);
        state->tray->setToolTip(recording ? QStringLiteral("VoiceToText - Recording...")
                                          : QStringLiteral("VoiceToText - Ready"));
        state->rebuild_menu();
    });
}

void TrayIcon::set_current_model(const std::string& model_size) {
    if (model_size.empty()) return;
    ui::run([state = state_, model_size]() {
        state->current_model_size = model_size;
        state->rebuild_menu();
    });
}

void TrayIcon::set_current_compute_mode(const std::string& whisper_mode,
                                        const std::string& gemma_mode) {
    ui::run([state = state_, whisper_mode, gemma_mode]() {
        if (!whisper_mode.empty()) state->current_whisper_compute_mode = whisper_mode;
        if (!gemma_mode.empty()) state->current_gemma_compute_mode = gemma_mode;
        state->rebuild_menu();
    });
}

void TrayIcon::set_current_gemma_model(const std::string& model_id,
                                       const std::string& recommended_model_id) {
    ui::run([state = state_, model_id, recommended_model_id]() {
        if (!model_id.empty()) state->current_gemma_model = model_id;
        if (!recommended_model_id.empty()) {
            state->recommended_gemma_model = recommended_model_id;
        }
        state->rebuild_menu();
    });
}

void TrayIcon::set_installed_gemma_models(const std::vector<std::string>& model_ids) {
    ui::run([state = state_, model_ids]() {
        state->installed_gemma_models = model_ids;
        state->rebuild_menu();
    });
}

void TrayIcon::set_tooltip(const std::string& tooltip) {
    ui::run([state = state_, tooltip]() {
        if (state->tray) state->tray->setToolTip(QString::fromStdString(tooltip));
    });
}

void TrayIcon::show_notification(const std::string& title, const std::string& message) {
    ui::run([state = state_, title, message]() {
        if (!state->tray) return;
        state->tray->showMessage(QString::fromStdString(title),
                                 QString::fromStdString(message),
                                 state->icon_idle, 5000);
    });
}

} // namespace vtt
