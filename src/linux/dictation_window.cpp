#include "dictation_window.h"

#include "app_icon.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWidget>

namespace vtt {
namespace {

const char* const WINDOW_STYLE = R"(
QWidget#VttDictation { background: #11171f; }
QLabel { color: #a5b4c3; background: transparent; }
QPlainTextEdit {
    background: #19222d;
    color: #e7edf4;
    border: 1px solid #485869;
    selection-background-color: #375c7d;
    selection-color: #ffffff;
}
QPushButton {
    background: #276596;
    color: #ffffff;
    border: 1px solid #4484b2;
    border-radius: 5px;
    padding: 6px 14px;
    font-weight: 600;
}
QPushButton:hover { background: #1b4e76; }
QPushButton:pressed { background: #1d4e76; }
QPushButton:disabled { background: #232d39; color: #6b7a89; border-color: #354250; }
)";

// The dictation buffer counts in characters (code points here); the Qt text
// document counts in UTF-16 units.
int utf16_position(const std::wstring& document, std::size_t position) {
    position = std::min(position, document.size());
    return static_cast<int>(QString::fromStdWString(document.substr(0, position)).size());
}

std::size_t character_position(const QString& document, int utf16_offset) {
    return document.left(utf16_offset).toStdWString().size();
}

} // namespace

class DictationWindow::Widget : public QWidget {
public:
    QLabel* hint = nullptr;
    QPlainTextEdit* editor = nullptr;
    QLabel* status = nullptr;
    QPushButton* stop_button = nullptr;
    QPushButton* copy_button = nullptr;
    QPushButton* accept_button = nullptr;
    std::function<void()> on_close;

    Widget() {
        setObjectName(QStringLiteral("VttDictation"));
        setWindowTitle(QString::fromUtf8(u8"Pianissimo – livediktering"));
        setWindowIcon(create_app_icon(QColor(0, 120, 215)));
        setStyleSheet(QString::fromUtf8(WINDOW_STYLE));
        setMinimumSize(640, 360);
        resize(760, 520);

        QFont text_font = QApplication::font();
        text_font.setPixelSize(14);
        QFont editor_font = text_font;
        editor_font.setPixelSize(16);

        hint = new QLabel(QString::fromUtf8(
            u8"Texten visas medan du talar. Markera text och fortsätt tala för att ersätta den. "
            u8"Du kan också skriva eller flytta markören."), this);
        hint->setWordWrap(true);
        hint->setFont(text_font);

        editor = new QPlainTextEdit(this);
        editor->setFont(editor_font);
        editor->document()->setDocumentMargin(12);

        status = new QLabel(QString::fromUtf8(u8"Redo för diktering."), this);
        status->setWordWrap(true);
        status->setFont(text_font);

        stop_button = new QPushButton(QStringLiteral("Stoppa"), this);
        copy_button = new QPushButton(QStringLiteral("Kopiera"), this);
        accept_button = new QPushButton(QStringLiteral("Infoga text"), this);
        accept_button->setDefault(true);
        for (QPushButton* button : {stop_button, copy_button, accept_button}) {
            button->setFont(text_font);
            button->setMinimumHeight(36);
        }

        auto* footer = new QHBoxLayout();
        footer->addWidget(status, 1);
        footer->addWidget(stop_button);
        footer->addWidget(copy_button);
        footer->addWidget(accept_button);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(10);
        layout->addWidget(hint);
        layout->addWidget(editor, 1);
        layout->addLayout(footer);
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        // Closing stops the dictation and hides the window; the app stays.
        if (on_close) on_close();
        hide();
        event->ignore();
    }
};

DictationWindow::~DictationWindow() {
    shutdown();
}

bool DictationWindow::initialize(HINSTANCE /*instance*/) {
    if (widget_) return true;
    widget_ = new Widget();
    widget_->on_close = [this]() { request_stop(); };

    QObject::connect(widget_->editor, &QPlainTextEdit::textChanged, widget_,
                     [this]() { observe_editor(); });
    QObject::connect(widget_->editor, &QPlainTextEdit::cursorPositionChanged, widget_,
                     [this]() { observe_editor(); });
    QObject::connect(widget_->editor, &QPlainTextEdit::selectionChanged, widget_,
                     [this]() { observe_editor(); });
    QObject::connect(widget_->stop_button, &QPushButton::clicked, widget_,
                     [this]() { request_stop(); });
    QObject::connect(widget_->copy_button, &QPushButton::clicked, widget_,
                     [this]() { copy_text(); });
    QObject::connect(widget_->accept_button, &QPushButton::clicked, widget_, [this]() {
        if (recording_ || processing_ || busy_) return;
        const auto text = text_utf8();
        if (!text.empty() && accept_callback_) accept_callback_(text);
    });

    update_buttons();
    return true;
}

void DictationWindow::shutdown() {
    delete widget_;
    widget_ = nullptr;
}

void DictationWindow::show(bool activate) {
    if (!widget_) return;
    widget_->show();
    if (activate) {
        widget_->raise();
        widget_->activateWindow();
        widget_->editor->setFocus();
    }
}

void DictationWindow::hide() {
    if (widget_) widget_->hide();
}

bool DictationWindow::is_visible() const {
    return widget_ && widget_->isVisible();
}

bool DictationWindow::owns_window(HWND /*window*/) const {
    return widget_ && widget_->isActiveWindow();
}

void DictationWindow::begin_session(bool clear) {
    if (!widget_) return;
    set_busy(false);
    applying_ = true;
    if (clear) widget_->editor->clear();
    insertion_.reset();
    applying_ = false;
    observe_editor();
    recording_ = true;
    processing_ = false;
    set_status(L"Lyssnar – texten uppdateras i realtid.");
    update_buttons();
}

void DictationWindow::update_transcript(int utterance_id, const std::string& text, bool is_final) {
    if (!widget_ || (!recording_ && !processing_)) return;
    const auto wide = QString::fromUtf8(text.data(), static_cast<int>(text.size())).toStdWString();
    if (!text.empty() && wide.empty()) return;

    QPlainTextEdit* editor = widget_->editor;
    const QString content = editor->toPlainText();
    const QTextCursor selection = editor->textCursor();
    const auto start = character_position(content, selection.selectionStart());
    const auto end = character_position(content, selection.selectionEnd());
    const auto current = content.toStdWString();
    const auto replacement = insertion_.apply(utterance_id, wide, is_final, current, start, end);
    if (!replacement) return;

    applying_ = true;
    QTextCursor cursor(editor->document());
    cursor.setPosition(utf16_position(current, replacement->start));
    cursor.setPosition(utf16_position(current, replacement->start + replacement->length),
                       QTextCursor::KeepAnchor);
    cursor.insertText(QString::fromStdWString(replacement->text));
    const auto updated = editor->toPlainText().toStdWString();
    cursor.setPosition(utf16_position(updated, replacement->caret));
    editor->setTextCursor(cursor);
    editor->ensureCursorVisible();
    applying_ = false;
    // No focus change here: recognition must not move focus.
    update_buttons();
}

void DictationWindow::finish_session() {
    insertion_.finish();
    recording_ = false;
    processing_ = false;
    set_status(L"Klart. Granska texten och välj Infoga text eller Kopiera.");
    update_buttons();
}

void DictationWindow::set_busy(bool busy) {
    busy_ = busy;
    if (widget_) widget_->editor->setReadOnly(busy);
    update_buttons();
}

void DictationWindow::set_status(const std::wstring& status) {
    if (widget_) widget_->status->setText(QString::fromStdWString(status));
}

std::string DictationWindow::text_utf8() const {
    return widget_ ? widget_->editor->toPlainText().toStdString() : std::string();
}

void DictationWindow::set_stop_callback(std::function<void()> callback) {
    stop_callback_ = std::move(callback);
}

void DictationWindow::set_accept_callback(std::function<void(const std::string&)> callback) {
    accept_callback_ = std::move(callback);
}

void DictationWindow::observe_editor() {
    if (applying_ || !widget_) return;
    const QString content = widget_->editor->toPlainText();
    const QTextCursor selection = widget_->editor->textCursor();
    insertion_.observe_editor(content.toStdWString(),
                              character_position(content, selection.selectionStart()),
                              character_position(content, selection.selectionEnd()));
    update_buttons();
}

void DictationWindow::update_buttons() {
    if (!widget_) return;
    widget_->stop_button->setEnabled(recording_);
    const bool has_text = !widget_->editor->document()->isEmpty();
    widget_->copy_button->setEnabled(has_text);
    widget_->accept_button->setEnabled(has_text && !recording_ && !processing_ && !busy_);
}

void DictationWindow::request_stop() {
    if (!recording_) return;
    recording_ = false;
    processing_ = true;
    set_status(L"Slutför de sista orden…");
    update_buttons();
    if (stop_callback_) stop_callback_();
    else finish_session();
}

void DictationWindow::copy_text() {
    if (!widget_) return;
    const QString text = widget_->editor->toPlainText();
    if (text.isEmpty()) return;
    QApplication::clipboard()->setText(text);
    set_status(L"Texten har kopierats.");
}

} // namespace vtt
