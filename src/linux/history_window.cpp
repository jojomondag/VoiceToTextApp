#include "history_window.h"

#include "app_icon.h"
#include "config_manager.h"
#include "transcript_history.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>

#include <algorithm>
#include <functional>
#include <vector>

namespace vtt {
namespace {

constexpr int CLEAR_BUTTON_WIDTH = 92;
constexpr int ENTRY_COPY_WIDTH = 24;
constexpr int ENTRY_COPY_HEIGHT = 22;
constexpr int PROFILE_BUTTON_WIDTH = 24;
constexpr int PROFILE_BUTTON_HEIGHT = 20;
constexpr int PROFILE_BUTTON_GAP = 6;
constexpr int SECTION_TITLE_INSET = 18;
constexpr int PANEL_INNER = 12;
constexpr int HISTORY_TEXT_LEFT_MARGIN = 54;
constexpr int HISTORY_TEXT_RIGHT_MARGIN = 8;
constexpr int ENTRY_TEXT_INSET = 8;
constexpr int ENTRY_GAP = 10;
constexpr qreal PANEL_RADIUS = 7.0;

const QColor COLOR_BG(17, 23, 31);
const QColor COLOR_PANEL(31, 40, 52);
const QColor COLOR_PANEL_EDGE(72, 88, 105);
const QColor COLOR_CONTROL(22, 29, 39);
const QColor COLOR_BUTTON(39, 101, 150);
const QColor COLOR_LABEL(232, 238, 244);
const QColor COLOR_MUTED(150, 164, 178);
const QColor COLOR_DATE(110, 170, 235);
const QColor COLOR_PROFILE(88, 210, 170);
const QColor COLOR_PROFILE_HOVER(36, 155, 122);
const QColor COLOR_TOKENS(230, 166, 80);
const QColor COLOR_TEXT(223, 230, 236);
const QColor COLOR_ENTRY_CARD(28, 38, 50);
const QColor COLOR_ENTRY_HIGHLIGHT(55, 92, 125);

QFont ui_font(int pixel_size, QFont::Weight weight = QFont::Normal) {
    QFont font = QApplication::font();
    font.setPixelSize(pixel_size);
    font.setWeight(weight);
    return font;
}

QFont label_font()  { return ui_font(16, QFont::DemiBold); }
QFont text_font()   { return ui_font(14); }
QFont button_font() { return ui_font(13, QFont::DemiBold); }

const char* const CONTROL_STYLE = R"(
QTextEdit, QPlainTextEdit {
    background: #161d27;
    color: #dfe6ec;
    border: none;
    selection-background-color: #375c7d;
    selection-color: #ffffff;
}
QComboBox {
    background: #161d27;
    color: #dfe6ec;
    border: 1px solid #485869;
    padding: 2px 8px;
    selection-background-color: #375c7d;
}
QComboBox:focus { border: 1px solid #4484b2; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView {
    background: #161d27;
    color: #dfe6ec;
    border: 1px solid #485869;
    selection-background-color: #375c7d;
    selection-color: #ffffff;
    outline: none;
}
QLabel { color: #96a4b2; background: transparent; }
QScrollBar:vertical { background: #161d27; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #485869; min-height: 28px; border-radius: 4px; }
QScrollBar::handle:vertical:hover { background: #5c7086; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
)";

// The blue "steel" button; without a label it shows a copy glyph. The green
// variant with a person glyph opens the profile editor.
class GlyphButton : public QPushButton {
public:
    enum class Kind { Steel, Profile };

    GlyphButton(Kind kind, const QString& text, QWidget* parent)
        : QPushButton(text, parent), kind_(kind) {
        setFont(button_font());
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(text.isEmpty() ? Qt::NoFocus : Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
    }

    // The history entry this button belongs to is highlighted.
    void set_highlighted(bool highlighted) {
        if (highlighted_ == highlighted) return;
        highlighted_ = highlighted;
        update();
    }

    std::function<void()> on_hover;

protected:
    void enterEvent(QEnterEvent* event) override {
        QPushButton::enterEvent(event);
        if (on_hover) on_hover();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        bool pressed = isDown();
        bool hot = underMouse();
        bool focused = hasFocus();
        QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        if (kind_ == Kind::Profile) {
            QColor fill = pressed ? QColor(39, 86, 74)
                : (hot ? QColor(24, 58, 50)
                       : (highlighted_ ? QColor(56, 110, 96) : QColor(35, 72, 64)));
            QColor border = highlighted_ ? QColor(145, 240, 210)
                : (focused ? QColor(120, 224, 190) : QColor(88, 170, 145));
            painter.setPen(border);
            painter.setBrush(fill);
            painter.drawRoundedRect(body, 4.5, 4.5);

            qreal cx = width() / 2.0;
            qreal cy = height() / 2.0 + (pressed ? 1 : 0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(235, 255, 248));
            painter.drawEllipse(QRectF(cx - 3.5, cy - 7.5, 7, 7));
            painter.drawRoundedRect(QRectF(cx - 7.5, cy + 1.5, 15, 6), 3, 3);
            return;
        }

        QColor fill = pressed ? QColor(29, 78, 118)
            : (hot ? QColor(27, 78, 118)
                   : (highlighted_ ? COLOR_ENTRY_HIGHLIGHT : COLOR_BUTTON));
        QColor border = highlighted_ ? QColor(115, 170, 215)
            : (focused ? QColor(120, 185, 235) : QColor(68, 132, 178));
        painter.setPen(border);
        painter.setBrush(fill);
        painter.drawRoundedRect(body, 5, 5);

        QRect content = rect();
        if (pressed) content.translate(0, 1);

        if (text().isEmpty()) {
            // Copy glyph: two overlapping sheets.
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setPen(QColor(245, 250, 255));
            painter.setBrush(Qt::NoBrush);
            QPoint c = content.center();
            painter.drawRect(QRect(c.x() - 5, c.y() - 5, 8, 8));
            painter.setBrush(fill);
            painter.drawRect(QRect(c.x() - 2, c.y() - 2, 8, 8));
            return;
        }

        painter.setPen(Qt::white);
        painter.setFont(font());
        painter.drawText(content, Qt::AlignCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight,
                                                  content.width() - 8));
    }

private:
    Kind kind_;
    bool highlighted_ = false;
};

// One history column: a read-only text view whose entries are cards with a
// coloured header line. A strip on the left holds each entry's copy button.
class HistoryColumn : public QTextEdit {
public:
    enum class Kind { Recorded, Transcribed, AiLanguage };

    explicit HistoryColumn(QWidget* parent) : QTextEdit(parent) {
        setReadOnly(true);
        setUndoRedoEnabled(false);
        setFrameShape(QFrame::NoFrame);
        setFont(text_font());
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setViewportMargins(HISTORY_TEXT_LEFT_MARGIN - ENTRY_TEXT_INSET, 0, 0, 0);
        document()->setDocumentMargin(0);
        viewport()->setMouseTracking(true);
    }

    // Entry index under the mouse (or one of its buttons); -1 when it left.
    std::function<void(int)> on_hover;
    // The profile link or button of an entry was clicked.
    std::function<void(const QString&)> on_profile;

    void set_entries(const std::vector<TranscriptHistoryEntry>& newest_first,
                     Kind kind) {
        for (auto& buttons : buttons_) {
            delete buttons.copy;
            delete buttons.profile;
        }
        buttons_.clear();
        entries_.clear();
        highlighted_ = -1;
        hovered_link_ = -1;
        viewport()->unsetCursor();

        QTextCharFormat plain;
        plain.setForeground(COLOR_TEXT);
        QTextCharFormat date = plain;
        date.setForeground(COLOR_DATE);
        QTextCharFormat profile = plain;
        profile.setForeground(COLOR_PROFILE);
        QTextCharFormat link = profile;
        link.setFontWeight(QFont::Bold);
        link.setFontUnderline(true);
        QTextCharFormat tokens = plain;
        tokens.setForeground(COLOR_TOKENS);

        document()->clear();
        QTextCursor cursor(document());
        cursor.beginEditBlock();

        for (size_t i = 0; i < newest_first.size(); ++i) {
            const TranscriptHistoryEntry& item = newest_first[i];
            const std::string& text =
                kind == Kind::Recorded ? item.recorded
                : (kind == Kind::Transcribed ? item.transcribed : item.ai_language);
            int token_count =
                kind == Kind::Recorded ? item.recorded_tokens
                : (kind == Kind::Transcribed ? item.transcribed_tokens
                                             : item.ai_language_tokens);

            QTextBlockFormat header;
            header.setBackground(COLOR_ENTRY_CARD);
            header.setLeftMargin(ENTRY_TEXT_INSET);
            header.setRightMargin(HISTORY_TEXT_RIGHT_MARGIN);
            header.setTopMargin(i == 0 ? 2 : ENTRY_GAP);
            if (i == 0) {
                cursor.setBlockFormat(header);
            } else {
                cursor.insertBlock(header, plain);
            }

            Entry entry;
            entry.start = cursor.position();
            entry.profile = QString::fromStdString(item.intent_profile);
            cursor.insertText(QLatin1Char('[') + QString::fromStdString(item.timestamp) +
                              QLatin1Char(']'), date);
            cursor.insertText(QStringLiteral("            "), plain);
            entry.profile_label = cursor.position();
            cursor.insertText(QStringLiteral("Profile: "), profile);
            entry.link_start = cursor.position();
            cursor.insertText(entry.profile, link);
            entry.link_end = cursor.position();
            cursor.insertText(QStringLiteral("    "), plain);
            cursor.insertText(QStringLiteral("Tokens: ") + QString::number(token_count),
                              tokens);

            entry.body = QString::fromStdString(text);
            entry.body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            while (entry.body.endsWith(QLatin1Char('\n')) ||
                   entry.body.endsWith(QLatin1Char(' ')) ||
                   entry.body.endsWith(QLatin1Char('\t'))) {
                entry.body.chop(1);
            }
            if (!entry.body.isEmpty()) {
                QTextBlockFormat body = header;
                body.setTopMargin(0);
                const QStringList lines = entry.body.split(QLatin1Char('\n'));
                for (const QString& line : lines) {
                    cursor.insertBlock(body, plain);
                    cursor.insertText(line, plain);
                }
            } else if (kind != Kind::Recorded && item.llm_pending) {
                // Shown, not copied: the language model has not answered yet.
                QTextBlockFormat body = header;
                body.setTopMargin(0);
                cursor.insertBlock(body, plain);
                cursor.insertText(QString::fromUtf8(u8"Väntar på språkmodellen..."),
                                  tokens);
            }
            entry.end = cursor.position();
            entries_.push_back(entry);
        }
        cursor.endEditBlock();

        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            EntryButtons buttons;
            buttons.copy = new GlyphButton(GlyphButton::Kind::Steel, QString(), this);
            buttons.copy->resize(ENTRY_COPY_WIDTH, ENTRY_COPY_HEIGHT);
            buttons.copy->on_hover = [this, i]() { if (on_hover) on_hover(i); };
            connect(buttons.copy, &QPushButton::clicked, this, [this, i]() {
                if (!entries_[i].body.isEmpty()) {
                    QApplication::clipboard()->setText(entries_[i].body);
                }
            });

            buttons.profile = new GlyphButton(GlyphButton::Kind::Profile, QString(),
                                              viewport());
            buttons.profile->resize(PROFILE_BUTTON_WIDTH, PROFILE_BUTTON_HEIGHT);
            buttons.profile->on_hover = [this, i]() { if (on_hover) on_hover(i); };
            connect(buttons.profile, &QPushButton::clicked, this, [this, i]() {
                if (on_profile) on_profile(entries_[i].profile);
            });
            buttons_.push_back(buttons);
        }
        update_buttons();
    }

    void set_highlight(int entry_index) {
        if (entry_index == highlighted_) return;
        paint_entry(highlighted_, COLOR_ENTRY_CARD);
        paint_entry(entry_index, COLOR_ENTRY_HIGHLIGHT);
        int previous = highlighted_;
        highlighted_ = entry_index;
        for (int index : {previous, highlighted_}) {
            if (index < 0 || index >= static_cast<int>(buttons_.size())) continue;
            buttons_[index].copy->set_highlighted(index == highlighted_);
            buttons_[index].profile->set_highlighted(index == highlighted_);
        }
    }

    // Scrolls so the entry sits about a third down the view.
    void center_entry(int entry_index) {
        if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) return;
        QScrollBar* bar = verticalScrollBar();
        int document_y = rect_at(entries_[entry_index].start).top() + bar->value();
        bar->setValue(document_y - viewport()->height() / 3);
    }

    void scroll_to_top() {
        verticalScrollBar()->setValue(0);
        update_buttons();
    }

    void update_buttons() {
        const int view_height = viewport()->height();
        const int view_width = viewport()->width();
        const QPoint view_origin = viewport()->pos();

        for (int i = 0; i < static_cast<int>(buttons_.size()); ++i) {
            const Entry& entry = entries_[i];
            EntryButtons& buttons = buttons_[i];

            QRect first_line = rect_at(entry.start);
            QRect last_line = rect_at(entry.end);
            int entry_top = first_line.top();
            int entry_bottom = std::max(last_line.bottom() + 1,
                                        entry_top + ENTRY_COPY_HEIGHT + 4);
            int y = entry_top + (entry_bottom - entry_top - ENTRY_COPY_HEIGHT) / 2;
            if (view_height <= ENTRY_COPY_HEIGHT + 8 ||
                y + ENTRY_COPY_HEIGHT < 2 || y > view_height - 2) {
                buttons.copy->hide();
                buttons.profile->hide();
                continue;
            }

            buttons.copy->move((HISTORY_TEXT_LEFT_MARGIN - ENTRY_COPY_WIDTH) / 2,
                               view_origin.y() + y);
            buttons.copy->show();

            QRect label = rect_at(entry.profile_label);
            int profile_x = label.left() - PROFILE_BUTTON_GAP - PROFILE_BUTTON_WIDTH;
            int profile_y = label.top() +
                            (label.height() - PROFILE_BUTTON_HEIGHT) / 2;
            // Keep it on screen while its header line is still partly visible.
            if (label.bottom() >= 0 && profile_y < 0) profile_y = 0;
            if (profile_x < 4 || profile_y < 0 ||
                profile_y + PROFILE_BUTTON_HEIGHT > view_height - 2 ||
                profile_x + PROFILE_BUTTON_WIDTH > view_width - 4) {
                buttons.profile->hide();
                continue;
            }
            buttons.profile->move(profile_x, profile_y);
            buttons.profile->show();
        }
    }

protected:
    bool viewportEvent(QEvent* event) override {
        switch (event->type()) {
            case QEvent::MouseMove: {
                auto* mouse = static_cast<QMouseEvent*>(event);
                hover_at(mouse->position().toPoint());
                break;
            }
            case QEvent::MouseButtonPress: {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton &&
                    link_at(mouse->position().toPoint()) >= 0) {
                    return true; // a click on the profile link is not a selection
                }
                break;
            }
            case QEvent::MouseButtonRelease: {
                auto* mouse = static_cast<QMouseEvent*>(event);
                int entry_index = link_at(mouse->position().toPoint());
                if (mouse->button() == Qt::LeftButton && entry_index >= 0 &&
                    !textCursor().hasSelection()) {
                    if (on_profile) on_profile(entries_[entry_index].profile);
                    return true;
                }
                break;
            }
            default:
                break;
        }
        return QTextEdit::viewportEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        QTextEdit::leaveEvent(event);
        set_link_hover(-1);
        if (on_hover) on_hover(-1);
    }

    void scrollContentsBy(int dx, int dy) override {
        QTextEdit::scrollContentsBy(dx, dy);
        update_buttons();
    }

    void resizeEvent(QResizeEvent* event) override {
        QTextEdit::resizeEvent(event);
        update_buttons();
    }

private:
    struct Entry {
        int start = 0;          // first character of the header line
        int end = 0;            // end of the last body line
        int profile_label = 0;  // "Profile:" in the header
        int link_start = 0;     // the profile name, a clickable link
        int link_end = 0;
        QString profile;
        QString body;
    };

    struct EntryButtons {
        GlyphButton* copy = nullptr;
        GlyphButton* profile = nullptr;
    };

    QRect rect_at(int position) const {
        QTextCursor cursor(document());
        cursor.setPosition(std::min(position, document()->characterCount() - 1));
        return cursorRect(cursor);
    }

    QPointF document_point(const QPoint& viewport_point) const {
        return QPointF(viewport_point.x() + horizontalScrollBar()->value(),
                       viewport_point.y() + verticalScrollBar()->value());
    }

    int entry_at(const QPoint& viewport_point) const {
        if (entries_.empty()) return -1;
        int position = document()->documentLayout()->hitTest(
            document_point(viewport_point), Qt::FuzzyHit);
        if (position < 0) return -1;
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (position >= entries_[i].start && position <= entries_[i].end) return i;
        }
        return -1;
    }

    // Entry whose profile link is exactly under the point, or -1.
    int link_at(const QPoint& viewport_point) const {
        int position = document()->documentLayout()->hitTest(
            document_point(viewport_point), Qt::ExactHit);
        if (position < 0) return -1;
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (position >= entries_[i].link_start && position < entries_[i].link_end) {
                return i;
            }
        }
        return -1;
    }

    void hover_at(const QPoint& viewport_point) {
        set_link_hover(link_at(viewport_point));
        if (on_hover) on_hover(entry_at(viewport_point));
    }

    void set_link_hover(int entry_index) {
        if (entry_index == hovered_link_) return;
        color_link(hovered_link_, COLOR_PROFILE);
        color_link(entry_index, COLOR_PROFILE_HOVER);
        hovered_link_ = entry_index;
        if (entry_index >= 0) {
            viewport()->setCursor(Qt::PointingHandCursor);
        } else {
            viewport()->unsetCursor();
        }
    }

    void color_link(int entry_index, const QColor& color) {
        if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) return;
        QTextCursor cursor(document());
        cursor.setPosition(entries_[entry_index].link_start);
        cursor.setPosition(entries_[entry_index].link_end, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setForeground(color);
        cursor.mergeCharFormat(format);
    }

    void paint_entry(int entry_index, const QColor& color) {
        if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) return;
        QTextCursor cursor(document());
        cursor.setPosition(entries_[entry_index].start);
        cursor.setPosition(entries_[entry_index].end, QTextCursor::KeepAnchor);
        QTextBlockFormat format;
        format.setBackground(color);
        cursor.mergeBlockFormat(format);
    }

    std::vector<Entry> entries_;
    std::vector<EntryButtons> buttons_;
    int highlighted_ = -1;
    int hovered_link_ = -1;
};

} // namespace

class HistoryWindow::Widget : public QWidget {
public:
    Widget(TranscriptHistory* history, ConfigManager* config)
        : history_(history), config_(config) {
        setWindowTitle(QStringLiteral("VoiceToText History"));
        setWindowIcon(create_app_icon(QColor(0, 120, 215)));
        setStyleSheet(QString::fromUtf8(CONTROL_STYLE));
        setMinimumSize(720, 360);
        resize(1260, 640);

        // --- History view ---
        for (HistoryColumn*& column : columns_) {
            column = new HistoryColumn(this);
            column->on_hover = [this, column](int entry_index) {
                highlight_entry(entry_index, column);
            };
            column->on_profile = [this](const QString& profile) {
                show_profile_prompt(profile.toStdString());
            };
        }

        clear_button_ = new GlyphButton(GlyphButton::Kind::Steel,
                                        QStringLiteral("Clear"), this);
        connect(clear_button_, &QPushButton::clicked, this, [this]() {
            auto answer = QMessageBox::warning(
                this, QStringLiteral("VoiceToText"),
                QStringLiteral("Clear all history entries? This cannot be undone."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes && history_) {
                history_->clear();
                refresh_entries();
            }
        });

        // --- Profile editor ---
        // Three sections mirror the LLM pipeline: the cleanup role applied to
        // the raw dictation, the profile that was chosen, and that profile's
        // own rewrite rules.
        prompt_pre_edit_ = new QPlainTextEdit(this);
        prompt_transform_edit_ = new QPlainTextEdit(this);
        for (QPlainTextEdit* edit : {prompt_pre_edit_, prompt_transform_edit_}) {
            edit->setFont(text_font());
            edit->setFrameShape(QFrame::NoFrame);
            edit->document()->setDocumentMargin(SECTION_TITLE_INSET - PANEL_INNER);
        }

        prompt_profile_combo_ = new QComboBox(this);
        prompt_profile_combo_->setEditable(true);
        prompt_profile_combo_->setInsertPolicy(QComboBox::NoInsert);
        prompt_profile_combo_->setFont(text_font());
        connect(prompt_profile_combo_, &QComboBox::activated, this, [this](int index) {
            if (index >= 0) {
                select_profile_for_editing(
                    prompt_profile_combo_->itemText(index).toStdString());
            }
        });

        prompt_profile_new_button_ = new GlyphButton(
            GlyphButton::Kind::Steel, QStringLiteral("New"), this);
        prompt_profile_rename_button_ = new GlyphButton(
            GlyphButton::Kind::Steel, QStringLiteral("Rename"), this);
        prompt_back_button_ = new GlyphButton(
            GlyphButton::Kind::Steel, QStringLiteral("Back"), this);
        prompt_save_button_ = new GlyphButton(
            GlyphButton::Kind::Steel, QStringLiteral("Save roles"), this);
        connect(prompt_profile_new_button_, &QPushButton::clicked, this,
                [this]() { create_new_profile(); });
        connect(prompt_profile_rename_button_, &QPushButton::clicked, this,
                [this]() { rename_current_profile(); });
        connect(prompt_back_button_, &QPushButton::clicked, this,
                [this]() { show_history_view(); });
        connect(prompt_save_button_, &QPushButton::clicked, this,
                [this]() { save_prompt_edits(); });

        prompt_status_ = new QLabel(this);
        prompt_status_->setFont(text_font());

        set_mode(false);
        refresh_entries();
    }

    void refresh_entries() {
        std::vector<TranscriptHistoryEntry> entries;
        if (history_) entries = history_->entries();
        std::reverse(entries.begin(), entries.end()); // newest first

        highlighted_entry_ = -1;
        columns_[0]->set_entries(entries, HistoryColumn::Kind::Recorded);
        columns_[1]->set_entries(entries, HistoryColumn::Kind::Transcribed);
        columns_[2]->set_entries(entries, HistoryColumn::Kind::AiLanguage);
        scroll_to_top();
    }

    void scroll_to_top() {
        for (HistoryColumn* column : columns_) column->scroll_to_top();
    }

protected:
    void resizeEvent(QResizeEvent*) override {
        layout_children();
    }

    void closeEvent(QCloseEvent* event) override {
        // Closing only hides the window; the app keeps running in the tray.
        hide();
        event->ignore();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), COLOR_BG);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);

        if (profile_mode_) {
            paint_section(painter, prompt_pre_panel_, QStringLiteral("Cleanup role"),
                          QStringLiteral("Repairs raw dictation before profile transform"));
            paint_section(painter, prompt_profile_panel_, QStringLiteral("Profile"),
                          QStringLiteral("Choose or edit the active transformation profile"));
            paint_section(painter, prompt_transform_panel_, QStringLiteral("Transform role"),
                          QStringLiteral("Profile-specific rewrite rules"));
        } else {
            paint_section(painter, panels_[0], QStringLiteral("Recorded voice"),
                          QStringLiteral("Raw capture before cleanup"));
            paint_section(painter, panels_[1], QStringLiteral("Clean transcript"),
                          QStringLiteral("Readable transcript output"));
            paint_section(painter, panels_[2], QStringLiteral("AI language"),
                          QStringLiteral("Compact instruction view"));
        }
    }

private:
    void paint_section(QPainter& painter, const QRect& rc, const QString& title,
                       const QString& subtitle) {
        if (rc.width() <= 0 || rc.height() <= 0) return;

        painter.setPen(COLOR_PANEL_EDGE);
        painter.setBrush(COLOR_PANEL);
        painter.drawRoundedRect(QRectF(rc).adjusted(0.5, 0.5, -0.5, -0.5),
                                PANEL_RADIUS, PANEL_RADIUS);

        painter.setPen(COLOR_LABEL);
        painter.setFont(label_font());
        QRect title_rc(rc.left() + SECTION_TITLE_INSET, rc.top() + 14,
                       rc.width() - SECTION_TITLE_INSET * 2, 22);
        painter.drawText(title_rc, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(title, Qt::ElideRight,
                                                          title_rc.width()));

        painter.setPen(COLOR_MUTED);
        painter.setFont(text_font());
        QRect subtitle_rc(rc.left() + SECTION_TITLE_INSET, rc.top() + 43,
                          rc.width() - SECTION_TITLE_INSET * 2, 21);
        painter.drawText(subtitle_rc, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(subtitle, Qt::ElideRight,
                                                          subtitle_rc.width()));
    }

    void set_mode(bool profile_mode) {
        profile_mode_ = profile_mode;
        for (HistoryColumn* column : columns_) column->setVisible(!profile_mode);
        clear_button_->setVisible(!profile_mode);
        for (QWidget* widget : std::initializer_list<QWidget*>{
                 prompt_pre_edit_, prompt_profile_combo_, prompt_profile_new_button_,
                 prompt_profile_rename_button_, prompt_transform_edit_,
                 prompt_back_button_, prompt_save_button_, prompt_status_}) {
            widget->setVisible(profile_mode);
        }
        layout_children();
        update();
    }

    void layout_children() {
        if (profile_mode_) {
            layout_prompt();
        } else {
            layout_history();
        }
        update();
    }

    void layout_history() {
        int padding = 22;
        int gap = 16;
        int panel_width = (width() - padding * 2 - gap * 2) / 3;
        int panel_top = padding;
        int panel_height = height() - padding * 2;
        int edit_margin = PANEL_INNER;
        int edit_top_offset = 68;
        int edit_height = panel_height - edit_top_offset - edit_margin;

        for (int i = 0; i < 3; ++i) {
            int x = padding + (panel_width + gap) * i;
            panels_[i] = QRect(x, panel_top, panel_width, panel_height);
            columns_[i]->setGeometry(x + edit_margin, panel_top + edit_top_offset,
                                     std::max(0, panel_width - edit_margin * 2),
                                     std::max(0, edit_height));
        }

        clear_button_->setGeometry(
            panels_[2].right() + 1 - edit_margin - CLEAR_BUTTON_WIDTH,
            panel_top + 16, CLEAR_BUTTON_WIDTH, 32);
    }

    void layout_prompt() {
        int padding = 24;
        int section_gap = 14;
        int footer_gap = 18;
        int button_height = 34;
        int button_width = 132;
        int short_height = 32; // profile controls row
        int small_button_width = 86;
        int control_gap = 10;

        int content_width = std::max(100, width() - padding * 2);
        int footer_top = height() - padding - button_height;
        int content_bottom = footer_top - footer_gap;
        int panel_header = 76;
        int panel_inner = SECTION_TITLE_INSET;
        int profile_panel_height = panel_header + panel_inner + short_height +
                                   panel_inner;
        int flex_total = content_bottom - padding - section_gap * 2 -
                         profile_panel_height -
                         (panel_header + panel_inner * 2) * 2;
        int flex_height = std::max(48, flex_total / 2);

        int y = padding;
        int pre_panel_height = panel_header + panel_inner + flex_height + panel_inner;
        prompt_pre_panel_ = QRect(padding, y, content_width, pre_panel_height);
        prompt_pre_edit_->setGeometry(padding + panel_inner, y + panel_header,
                                      content_width - panel_inner * 2, flex_height);
        y += pre_panel_height + section_gap;

        prompt_profile_panel_ = QRect(padding, y, content_width, profile_panel_height);
        int profile_controls_y = y + panel_header;
        int profile_x = padding + panel_inner;
        int profile_buttons_width = small_button_width * 2 + control_gap * 2;
        int combo_width = std::max(
            160, content_width - panel_inner * 2 - profile_buttons_width);
        prompt_profile_combo_->setGeometry(profile_x, profile_controls_y,
                                           combo_width, short_height);
        profile_x += combo_width + control_gap;
        prompt_profile_new_button_->setGeometry(profile_x, profile_controls_y,
                                                small_button_width, short_height);
        profile_x += small_button_width + control_gap;
        prompt_profile_rename_button_->setGeometry(profile_x, profile_controls_y,
                                                   small_button_width, short_height);
        y += profile_panel_height + section_gap;

        int transform_panel_height = panel_header + panel_inner + flex_height +
                                     panel_inner;
        prompt_transform_panel_ = QRect(padding, y, content_width,
                                        transform_panel_height);
        prompt_transform_edit_->setGeometry(padding + panel_inner, y + panel_header,
                                            content_width - panel_inner * 2,
                                            flex_height);

        int button_x = width() - padding - button_width;
        prompt_back_button_->setGeometry(padding, footer_top, button_width,
                                         button_height);
        prompt_save_button_->setGeometry(button_x, footer_top, button_width,
                                         button_height);
        prompt_status_->setGeometry(
            padding + button_width + control_gap, footer_top,
            std::max(0, button_x - padding - button_width - control_gap * 2),
            button_height);
    }

    // Marks one entry in all three columns and brings it into view in the
    // columns the mouse is not over.
    void highlight_entry(int entry_index, HistoryColumn* source) {
        if (entry_index == highlighted_entry_) return;
        highlighted_entry_ = entry_index;
        for (HistoryColumn* column : columns_) {
            column->set_highlight(entry_index);
            if (entry_index >= 0 && column != source) {
                column->center_entry(entry_index);
            }
            column->update_buttons();
        }
    }

    void populate_profile_combo(const std::string& selected) {
        if (!config_) return;

        prompt_profile_combo_->clear();
        for (const auto& profile : config_->config().llm_profiles) {
            prompt_profile_combo_->addItem(QString::fromStdString(profile.name));
        }

        QString profile_name =
            QString::fromStdString(selected.empty() ? "general" : selected);
        prompt_profile_combo_->setCurrentIndex(
            prompt_profile_combo_->findText(profile_name));
        prompt_profile_combo_->setEditText(profile_name);
    }

    void select_profile_for_editing(const std::string& profile) {
        if (!config_) return;

        std::string profile_name = profile.empty() ? "general" : profile;
        const auto& profiles = config_->config().llm_profiles;
        auto it = std::find_if(profiles.begin(), profiles.end(),
                               [&](const ProfileDefinition& item) {
                                   return item.name == profile_name;
                               });
        if (it == profiles.end()) {
            it = std::find_if(profiles.begin(), profiles.end(),
                              [](const ProfileDefinition& item) {
                                  return item.name == "general";
                              });
        }
        if (it == profiles.end() && !profiles.empty()) {
            it = profiles.begin();
        }

        if (it == profiles.end()) return;
        active_prompt_profile_ = it->name;
        QString name = QString::fromStdString(it->name);
        prompt_profile_combo_->setCurrentIndex(prompt_profile_combo_->findText(name));
        prompt_profile_combo_->setEditText(name);
        prompt_transform_edit_->setPlainText(QString::fromStdString(it->instructions));
    }

    std::string current_profile_name_from_ui() const {
        return prompt_profile_combo_->currentText().trimmed().toStdString();
    }

    void create_new_profile() {
        if (!config_) return;

        auto& profiles = config_->config().llm_profiles;
        std::string base = "new-profile";
        std::string name = base;
        int suffix = 2;
        auto exists = [&](const std::string& candidate) {
            return std::any_of(profiles.begin(), profiles.end(),
                               [&](const ProfileDefinition& item) {
                                   return item.name == candidate;
                               });
        };
        while (exists(name)) {
            name = base + "-" + std::to_string(suffix++);
        }

        profiles.push_back({
            name,
            u8"Beskriv hur den här profilen ska skriva om texten."
        });
        populate_profile_combo(name);
        select_profile_for_editing(name);
        config_->save();
        prompt_status_->setText(
            QStringLiteral("New profile created. Edit instructions, then save."));
        prompt_profile_combo_->setFocus();
    }

    void rename_current_profile() {
        if (!config_) return;

        std::string new_name = current_profile_name_from_ui();
        if (new_name.empty() || active_prompt_profile_.empty()) {
            prompt_status_->setText(
                QStringLiteral("Type a profile name before renaming."));
            return;
        }

        auto& profiles = config_->config().llm_profiles;
        auto active = std::find_if(profiles.begin(), profiles.end(),
                                   [&](const ProfileDefinition& item) {
                                       return item.name == active_prompt_profile_;
                                   });
        if (active == profiles.end()) return;

        auto duplicate = std::find_if(profiles.begin(), profiles.end(),
                                      [&](const ProfileDefinition& item) {
                                          return item.name == new_name &&
                                                 item.name != active_prompt_profile_;
                                      });
        if (duplicate != profiles.end()) {
            prompt_status_->setText(
                QStringLiteral("That profile name already exists."));
            return;
        }

        active->name = new_name;
        active_prompt_profile_ = new_name;
        populate_profile_combo(new_name);
        config_->save();
        prompt_status_->setText(QStringLiteral("Profile renamed."));
    }

    void show_profile_prompt(const std::string& profile) {
        if (profile.empty()) return;

        if (config_) {
            prompt_pre_edit_->setPlainText(
                QString::fromStdString(config_->config().llm_pre_prompt));
            populate_profile_combo(profile);
            select_profile_for_editing(profile);
        }
        prompt_status_->clear();

        set_mode(true);
        prompt_pre_edit_->setFocus();
    }

    void show_history_view() {
        set_mode(false);
        for (HistoryColumn* column : columns_) column->update_buttons();
        setFocus();
    }

    void save_prompt_edits() {
        if (!config_) return;

        std::string pre = prompt_pre_edit_->toPlainText().toStdString();
        std::string transform = prompt_transform_edit_->toPlainText().toStdString();
        std::string profile_name = current_profile_name_from_ui();
        if (profile_name.empty()) {
            prompt_status_->setText(
                QStringLiteral("Type a profile name before saving."));
            return;
        }

        auto& cfg = config_->config();
        cfg.llm_pre_prompt = pre;
        auto profile = std::find_if(cfg.llm_profiles.begin(), cfg.llm_profiles.end(),
                                    [&](const ProfileDefinition& item) {
                                        return item.name == profile_name;
                                    });
        if (profile == cfg.llm_profiles.end()) {
            cfg.llm_profiles.push_back({profile_name, transform});
        } else {
            profile->instructions = transform;
        }
        active_prompt_profile_ = profile_name;

        if (!config_->save()) {
            prompt_status_->setText(
                QStringLiteral("Save failed - check write permissions."));
            return;
        }
        populate_profile_combo(profile_name);
        prompt_status_->setText(
            QStringLiteral("Roles and profile saved to config.json."));
    }

    TranscriptHistory* history_ = nullptr;
    ConfigManager* config_ = nullptr;

    HistoryColumn* columns_[3] = {};
    QRect panels_[3];
    GlyphButton* clear_button_ = nullptr;
    int highlighted_entry_ = -1;

    bool profile_mode_ = false;
    QRect prompt_pre_panel_;
    QRect prompt_profile_panel_;
    QRect prompt_transform_panel_;
    QPlainTextEdit* prompt_pre_edit_ = nullptr;
    QComboBox* prompt_profile_combo_ = nullptr;
    GlyphButton* prompt_profile_new_button_ = nullptr;
    GlyphButton* prompt_profile_rename_button_ = nullptr;
    QPlainTextEdit* prompt_transform_edit_ = nullptr;
    GlyphButton* prompt_back_button_ = nullptr;
    GlyphButton* prompt_save_button_ = nullptr;
    QLabel* prompt_status_ = nullptr;
    std::string active_prompt_profile_;
};

HistoryWindow::HistoryWindow() = default;

HistoryWindow::~HistoryWindow() {
    shutdown();
}

bool HistoryWindow::initialize(HINSTANCE /*hInstance*/,
                               TranscriptHistory* history,
                               ConfigManager* config) {
    if (!widget_) {
        widget_ = new Widget(history, config);
    }
    return true;
}

void HistoryWindow::shutdown() {
    delete widget_;
    widget_ = nullptr;
}

void HistoryWindow::show() {
    if (!widget_) return;
    widget_->refresh_entries();
    // A window that is already open somewhere is mapped again, so it turns up
    // focused on the workspace the user is looking at.
    if (widget_->isVisible()) widget_->hide();
    widget_->show();
    widget_->raise();
    widget_->activateWindow();
    widget_->scroll_to_top();
}

void HistoryWindow::refresh() {
    if (widget_) widget_->refresh_entries();
}

} // namespace vtt
