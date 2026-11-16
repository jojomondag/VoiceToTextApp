#include "overlay_window.h"

#include "../../resources/version.h"
#include "log.h"
#include "platform.h"
#include "ui_thread.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#ifdef VTT_HAVE_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <vector>

namespace vtt {
namespace {

constexpr int OVERLAY_WIDTH = 500;
constexpr int OVERLAY_HEIGHT = 140;
constexpr int OVERLAY_COMPACT_HEIGHT = 60;
constexpr int OVERLAY_QUEUED_HEIGHT = 86; // compact plus the queued-start line
constexpr int QUEUED_LINE_HEIGHT = 20;
constexpr int OVERLAY_TOP = 20;
constexpr int DOT_RADIUS = 10;
constexpr int CORNER_RADIUS = 20;
constexpr int PADDING = 18;
constexpr int WAVEFORM_HEIGHT = 28;
constexpr int WAVEFORM_BARS = 28;
constexpr int VERSION_TEXT_HEIGHT = 18;
constexpr int VERSION_TEXT_GAP = 4;
constexpr int PASTE_BUTTON_COUNT = 3;
constexpr int MODE_ICON_SIZE = 42;
constexpr int MODE_ICON_GAP = 8;
constexpr int MODE_HIT_PADDING = 4;
constexpr int MODE_ICON_TOTAL_WIDTH =
    PASTE_BUTTON_COUNT * MODE_ICON_SIZE + (PASTE_BUTTON_COUNT - 1) * MODE_ICON_GAP;
constexpr int TOOLTIP_WIDTH = 360;
constexpr int TOOLTIP_DELAY_MS = 400;

const char* const PASTE_MODES[PASTE_BUTTON_COUNT] = {"recorded", "ai", "clean"};
const char* const PASTE_MODE_ICONS[PASTE_BUTTON_COUNT] = {
    "overlay_voice_mode.png", "overlay_ai_language_mode.png", "overlay_cleanup_mode.png"};
const char* const PASTE_MODE_TOOLTIPS[PASTE_BUTTON_COUNT] = {
    u8"Rösttext\n"
    u8"Klicka för att återställa originalet i det aktuella orörda textstycket.\n\n"
    u8"Fortsatt tal skrivs som originaltext. Text du själv redigerar bevaras.",
    u8"AI-språk\n"
    u8"Klicka för att göra det aktuella orörda textstycket till en tydlig AI-prompt "
    u8"med mål, uppgifter och krav. Fortsatt tal bygger vidare på prompten.\n\n"
    u8"Oklara delar markeras utan att fyllas i med gissningar. "
    u8"Nya ord kan synas som originaltext en kort stund medan Gemma bearbetar dem. "
    u8"Vänta på AI klar innan du markerar eller kopierar. Stopp slutför bearbetningen. "
    u8"Text du själv redigerar bevaras.",
    u8"Städa text\n"
    u8"Klicka för att rätta stavning, skiljetecken och grammatik i det aktuella "
    u8"orörda textstycket och fortsätta städa texten när du pratar.\n\n"
    u8"Nya ord kan synas som originaltext en kort stund medan Gemma bearbetar dem. "
    u8"Text du själv redigerar bevaras.",
};

const QColor COLOR_LISTENING(66, 133, 244);
const QColor COLOR_PROCESSING(251, 188, 4);
const QColor COLOR_ERROR(234, 67, 53);
const QColor COLOR_TEXT(255, 255, 255);

const char* const QUEUED_START_TEXT = u8"Ny diktering startar efteråt · tryck igen för att ångra";

float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

QColor with_alpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

std::string normalize_paste_mode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode == "ai" || mode == "ai_language" || mode == "ai-language") return "ai";
    if (mode == "recorded" || mode == "recorded_voice" || mode == "voice") return "recorded";
    return "clean";
}

// The mode icons ship next to the executable (build tree) or under
// share/voicetotext (installed).
QImage load_resource_image(const char* filename) {
    const std::filesystem::path exe_dir = platform::exe_directory();
    const std::filesystem::path candidates[] = {
        exe_dir / "resources" / filename,
        exe_dir.parent_path() / "share" / "voicetotext" / "resources" / filename,
        std::filesystem::path("resources") / filename,
    };
    for (const auto& path : candidates) {
        QImage image(QString::fromStdString(path.string()));
        if (!image.isNull()) return image;
    }
    return {};
}

// The selected look of an icon is the image itself; the others are shown in
// grey at 22 % opacity (the colour matrix of the Windows build).
QImage faded(const QImage& source) {
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int grey = static_cast<int>(0.299 * qRed(line[x]) + 0.587 * qGreen(line[x]) +
                                              0.114 * qBlue(line[x]));
            line[x] = qRgba(grey, grey, grey, static_cast<int>(qAlpha(line[x]) * 0.22));
        }
    }
    return image;
}

#ifdef VTT_HAVE_LAYER_SHELL
bool attach_layer_surface(QWidget* widget, const QString& scope, int top_margin) {
    if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) return false;
    widget->winId(); // create the QWindow the layer surface is attached to
    auto* layer = LayerShellQt::Window::get(widget->windowHandle());
    if (!layer) return false;
    layer->setScope(scope);
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(LayerShellQt::Window::AnchorTop);
    layer->setMargins(QMargins(0, top_margin, 0, 0));
    layer->setExclusiveZone(0);
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    layer->setActivateOnShow(false);
    // The screen the user is working on, not a fixed one.
    layer->setWantsToBeOnActiveScreen(true);
    return true;
}
#else
bool attach_layer_surface(QWidget*, const QString&, int) { return false; }
#endif

// Hover explanation for a mode button: its own small surface under the pill.
class TooltipWidget : public QWidget {
public:
    TooltipWidget() {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                       Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput |
                       Qt::ToolTip);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        QFont tip_font = font();
        tip_font.setPixelSize(13);
        setFont(tip_font);
        layer_shell_ = attach_layer_surface(this, QStringLiteral("voicetotext-tooltip"),
                                            OVERLAY_TOP + OVERLAY_HEIGHT + 8);
    }

    void present(const QString& text) {
        text_ = text;
        const QRect bounds = fontMetrics().boundingRect(
            QRect(0, 0, TOOLTIP_WIDTH - 24, 2000), Qt::TextWordWrap, text_);
        setFixedSize(TOOLTIP_WIDTH, bounds.height() + 22);
        if (!layer_shell_) {
            if (QScreen* screen = QGuiApplication::primaryScreen()) {
                const QRect area = screen->geometry();
                move(area.x() + (area.width() - TOOLTIP_WIDTH) / 2,
                     area.y() + OVERLAY_TOP + OVERLAY_HEIGHT + 8);
            }
        }
        if (!isVisible()) show();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.setCompositionMode(QPainter::CompositionMode_Source);
        g.fillRect(rect(), Qt::transparent);
        g.setCompositionMode(QPainter::CompositionMode_SourceOver);
        g.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
        g.setBrush(QColor(32, 33, 36, 245));
        g.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        g.setPen(QColor(235, 238, 242));
        g.drawText(rect().adjusted(12, 10, -12, -10), Qt::TextWordWrap, text_);
    }

private:
    QString text_;
    bool layer_shell_ = false;
};

class OverlayWidget : public QWidget {
public:
    OverlayWidget() {
        setWindowTitle(QStringLiteral("VoiceToText Overlay"));
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                       Qt::WindowDoesNotAcceptFocus | Qt::Tool);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setFixedSize(OVERLAY_WIDTH, OVERLAY_HEIGHT);
        clock_.start();

        for (int i = 0; i < PASTE_BUTTON_COUNT; ++i) {
            const int x = OVERLAY_WIDTH - PADDING - MODE_ICON_TOTAL_WIDTH +
                          i * (MODE_ICON_SIZE + MODE_ICON_GAP);
            const int y = PADDING - 4;
            button_rects_[i] = QRect(x - MODE_HIT_PADDING, y - MODE_HIT_PADDING,
                                     MODE_ICON_SIZE + MODE_HIT_PADDING * 2,
                                     MODE_ICON_SIZE + MODE_HIT_PADDING * 2);
            icons_[i] = load_resource_image(PASTE_MODE_ICONS[i]);
            if (!icons_[i].isNull()) faded_icons_[i] = faded(icons_[i]);
        }

        tooltip_timer_.setSingleShot(true);
        QObject::connect(&tooltip_timer_, &QTimer::timeout, this, [this]() {
            if (state_ == OverlayState::Listening && hover_button_ >= 0) {
                tooltip_.present(QString::fromUtf8(PASTE_MODE_TOOLTIPS[hover_button_]));
            }
        });

        layer_shell_ = attach_layer_surface(this, QStringLiteral("voicetotext-overlay"),
                                            OVERLAY_TOP);
    }

    std::function<void(const std::string&)> on_mode_selected;
    // Mirrors queued_start_ for OverlayWindow::queued_start_shown().
    std::atomic<bool>* queued_start_flag = nullptr;

    void present(OverlayState state, const std::string& text) {
        dismiss_tooltip();
        if (state != OverlayState::Processing) clear_queued_start("state changed");
        state_ = state;
        text_ = QString::fromStdString(text);
        progress_ = 0.0f;
        if (state == OverlayState::Listening) {
            audio_levels_.clear();
        }

        if (state == OverlayState::Hidden) {
            dismiss();
            return;
        }

        resize_for_state();

        if (!layer_shell_) {
            // No layer shell: best effort, top centre of the primary screen.
            if (QScreen* screen = QGuiApplication::primaryScreen()) {
                const QRect area = screen->geometry();
                move(area.x() + (area.width() - OVERLAY_WIDTH) / 2, area.y() + OVERLAY_TOP);
            }
        }
        if (!isVisible()) QWidget::show();
        update_input_region();
        update();
    }

    void dismiss() {
        dismiss_tooltip();
        clear_queued_start("overlay hidden");
        state_ = OverlayState::Hidden;
        hover_button_ = -1;
        if (isVisible()) QWidget::hide();
    }

    void set_audio_level(float level) {
        level = clampf(level, 0.0f, 1.0f);

        // Raw microphone RMS is usually tiny (speech is often around
        // 0.003-0.01), so map it into a UI meter range instead of drawing the
        // raw value.
        const float meter = clampf((level - 0.0005f) * 140.0f, 0.0f, 1.0f);
        const float alpha = meter > audio_level_ ? 0.80f : 0.35f; // fast attack, slower release
        audio_level_ = (audio_level_ * (1.0f - alpha)) + (meter * alpha);
        audio_levels_.push_back(audio_level_);
        if (audio_levels_.size() > WAVEFORM_BARS) {
            audio_levels_.erase(audio_levels_.begin());
        }
        if (isVisible()) update();
    }

    void set_progress(float progress, const std::string& text) {
        progress_ = clampf(progress, 0.0f, 1.0f);
        if (!text.empty()) text_ = QString::fromStdString(text);
        if (isVisible()) update();
    }

    void set_status_text(const std::string& text) {
        const QString next = QString::fromStdString(text);
        if (text_ == next) return;
        text_ = next;
        if (isVisible()) update();
    }

    void set_paste_output_mode(const std::string& mode) {
        paste_output_mode_ = normalize_paste_mode(mode);
        if (isVisible()) update();
    }

    // Only marked while the Processing pill is on screen; otherwise nobody
    // would see it, and the caller treats the start as not shown.
    void set_queued_start(bool queued) {
        if (!queued) {
            clear_queued_start("withdrawn");
            return;
        }
        if (!isVisible() || state_ != OverlayState::Processing) {
            vtlog("overlay: queued start not shown (visible=%d state=%d)",
                  isVisible() ? 1 : 0, static_cast<int>(state_));
            return;
        }
        if (queued_start_) return;
        queued_start_ = true;
        if (queued_start_flag) queued_start_flag->store(true);
        resize_for_state();
        update();
        vtlog("overlay: queued start shown (pill %dx%d)", width(), height());
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        if (state_ != OverlayState::Listening) return;
        set_hover(hit_test(event->position().toPoint()));
    }

    void leaveEvent(QEvent*) override {
        set_hover(-1);
    }

    void mousePressEvent(QMouseEvent* event) override {
        dismiss_tooltip();
        if (state_ != OverlayState::Listening || event->button() != Qt::LeftButton) return;
        const int hit = hit_test(event->position().toPoint());
        if (hit < 0) return;
        paste_output_mode_ = PASTE_MODES[hit];
        update();
        if (on_mode_selected) on_mode_selected(paste_output_mode_);
    }

    void paintEvent(QPaintEvent*) override {
        const int overlay_height = height();

        QColor dot_color = COLOR_LISTENING;
        QString display_text = text_;
        switch (state_) {
            case OverlayState::Listening:
                dot_color = COLOR_LISTENING;
                if (display_text.isEmpty()) display_text = QStringLiteral("Listening...");
                break;
            case OverlayState::Processing:
                dot_color = COLOR_PROCESSING;
                if (display_text.isEmpty()) display_text = QStringLiteral("Processing...");
                break;
            case OverlayState::Downloading:
                dot_color = COLOR_PROCESSING;
                if (display_text.isEmpty()) display_text = QStringLiteral("Downloading...");
                break;
            case OverlayState::Error:
                dot_color = COLOR_ERROR;
                if (display_text.isEmpty()) display_text = QStringLiteral("Error");
                break;
            case OverlayState::Hidden:
                break;
        }

        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.setRenderHint(QPainter::TextAntialiasing);
        g.setRenderHint(QPainter::SmoothPixmapTransform);
        g.setCompositionMode(QPainter::CompositionMode_Source);
        g.fillRect(rect(), Qt::transparent);
        g.setCompositionMode(QPainter::CompositionMode_SourceOver);

        g.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
        g.setBrush(QColor(32, 33, 36, 235));
        g.drawRoundedRect(QRectF(0.5, 0.5, OVERLAY_WIDTH - 1, overlay_height - 1),
                          CORNER_RADIUS, CORNER_RADIUS);

        const float seconds = static_cast<float>(clock_.elapsed() % 100000) / 1000.0f;
        const float pulse = (std::sin(seconds * 4.2f) + 1.0f) * 0.5f;

        QFont font = this->font();
        font.setBold(true);
        font.setPixelSize(20);
        g.setFont(font);
        const qreal text_width = g.fontMetrics().horizontalAdvance(display_text);

        const qreal dot_x = PADDING + DOT_RADIUS;
        const qreal left_bound = dot_x + DOT_RADIUS + 12.0;
        qreal right_bound = state_ == OverlayState::Listening
            ? OVERLAY_WIDTH - PADDING - MODE_ICON_TOTAL_WIDTH - 8.0
            : OVERLAY_WIDTH - PADDING;
        if (right_bound <= left_bound) right_bound = left_bound + 1.0;
        const qreal available = right_bound - left_bound;
        const qreal use_width = std::min(text_width, available);
        const bool centred = state_ == OverlayState::Listening ||
                             state_ == OverlayState::Processing;
        const qreal text_left = centred ? left_bound + (available - use_width) / 2.0
                                        : left_bound;

        const qreal dot_y = PADDING + DOT_RADIUS + 1;
        if (state_ == OverlayState::Listening) {
            const qreal halo_radius = DOT_RADIUS + 3.5 + pulse * 4.5;
            const int halo_alpha = static_cast<int>(45 + pulse * 50);
            g.setPen(QPen(with_alpha(COLOR_LISTENING, halo_alpha), 2.0));
            g.setBrush(Qt::NoBrush);
            g.drawEllipse(QPointF(dot_x, dot_y), halo_radius, halo_radius);
        }

        g.setPen(Qt::NoPen);
        g.setBrush(dot_color);
        g.drawEllipse(QPointF(dot_x, dot_y), DOT_RADIUS, DOT_RADIUS);

        g.setPen(COLOR_TEXT);
        g.drawText(QRectF(text_left, PADDING - 1.0, use_width + 1.0, 28.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   g.fontMetrics().elidedText(display_text, Qt::ElideRight,
                                              static_cast<int>(use_width + 1.0)));

        if (state_ == OverlayState::Processing && queued_start_) {
            // The next dictation waits for this one: a small blue dot (the
            // listening colour) and one line under the processing text.
            const qreal line_y = OVERLAY_COMPACT_HEIGHT - 6.0;
            g.setPen(Qt::NoPen);
            g.setBrush(COLOR_LISTENING);
            g.drawEllipse(QPointF(dot_x, line_y + QUEUED_LINE_HEIGHT / 2.0), 5.0, 5.0);
            QFont queued_font = this->font();
            queued_font.setPixelSize(14);
            g.setFont(queued_font);
            g.setPen(with_alpha(COLOR_TEXT, 220));
            const QRectF line_rect(left_bound, line_y, OVERLAY_WIDTH - PADDING - left_bound,
                                   QUEUED_LINE_HEIGHT);
            g.drawText(line_rect, Qt::AlignLeft | Qt::AlignVCenter,
                       g.fontMetrics().elidedText(QString::fromUtf8(QUEUED_START_TEXT),
                                                  Qt::ElideRight,
                                                  static_cast<int>(line_rect.width())));
            g.setFont(font);
        }

        if (state_ == OverlayState::Downloading) {
            const qreal bar_x = PADDING;
            const qreal bar_y = OVERLAY_HEIGHT - PADDING - 22;
            const qreal bar_w = OVERLAY_WIDTH - PADDING * 2;
            const qreal bar_h = 22.0;

            g.setPen(Qt::NoPen);
            g.setBrush(QColor(48, 49, 54, 150));
            g.drawRoundedRect(QRectF(bar_x, bar_y, bar_w, bar_h), 9.0, 9.0);

            const qreal fill_w = bar_w * progress_;
            if (fill_w > 1.0) {
                // Clip to the track so a short fill keeps the rounded ends.
                QPainterPath track;
                track.addRoundedRect(QRectF(bar_x, bar_y, bar_w, bar_h), 9.0, 9.0);
                g.save();
                g.setClipPath(track);
                g.setBrush(with_alpha(COLOR_PROCESSING, 230));
                g.drawRect(QRectF(bar_x, bar_y, fill_w, bar_h));
                g.restore();
            }

            const int percent = static_cast<int>(progress_ * 100.0f + 0.5f);
            QFont percent_font = font;
            percent_font.setPixelSize(13);
            g.setFont(percent_font);
            g.setPen(with_alpha(COLOR_TEXT, 235));
            g.drawText(QRectF(bar_x, bar_y, bar_w, bar_h), Qt::AlignCenter,
                       QString::number(percent) + QLatin1Char('%'));
        } else if (state_ == OverlayState::Listening) {
            for (int i = 0; i < PASTE_BUTTON_COUNT; ++i) {
                const qreal x = button_rects_[i].left() + MODE_HIT_PADDING;
                const qreal y = button_rects_[i].top() + MODE_HIT_PADDING;
                const bool selected = paste_output_mode_ == PASTE_MODES[i];
                const bool hovered = hover_button_ == i;
                if (selected || hovered) {
                    g.setPen(Qt::NoPen);
                    g.setBrush(with_alpha(COLOR_LISTENING, selected ? 85 : 45));
                    g.drawEllipse(QRectF(x - 3.0, y - 3.0, MODE_ICON_SIZE + 6.0,
                                         MODE_ICON_SIZE + 6.0));
                }
                const QRectF icon_rect(x, y, MODE_ICON_SIZE, MODE_ICON_SIZE);
                if (!icons_[i].isNull()) {
                    g.drawImage(icon_rect, selected ? icons_[i] : faded_icons_[i]);
                } else {
                    draw_fallback_icon(g, i, icon_rect,
                                       selected ? QColor(255, 255, 255)
                                                : QColor(255, 255, 255, 55));
                }
            }

            const qreal graph_x = PADDING;
            const qreal graph_y = OVERLAY_HEIGHT - PADDING - VERSION_TEXT_HEIGHT -
                                  VERSION_TEXT_GAP - WAVEFORM_HEIGHT;
            const qreal graph_w = OVERLAY_WIDTH - PADDING * 2;
            const qreal graph_h = WAVEFORM_HEIGHT;

            g.setPen(Qt::NoPen);
            g.setBrush(QColor(48, 49, 54, 150));
            g.drawRoundedRect(QRectF(graph_x, graph_y, graph_w, graph_h), 8.0, 8.0);

            const qreal gap = 4.0;
            qreal bar_width = (graph_w - (WAVEFORM_BARS - 1) * gap) / WAVEFORM_BARS;
            if (bar_width < 4.0) bar_width = 4.0;
            const qreal center_y = graph_y + graph_h / 2.0;

            const QColor bar_color = with_alpha(COLOR_LISTENING, 235);
            const QColor quiet_color(95, 125, 185, 150);
            for (int i = 0; i < WAVEFORM_BARS; ++i) {
                const int history_offset =
                    static_cast<int>(audio_levels_.size()) - WAVEFORM_BARS + i;
                float value = history_offset >= 0
                    ? audio_levels_[static_cast<size_t>(history_offset)] : 0.0f;
                const float idle_wave = 0.08f + 0.045f *
                    (std::sin(seconds * 5.0f + i * 0.72f) + 1.0f) * 0.5f;
                value = clampf(std::max(value, idle_wave), 0.03f, 1.0f);

                const qreal bar_height = std::max(5, static_cast<int>(value * graph_h));
                const qreal x = graph_x + i * (bar_width + gap);
                g.setBrush(value > 0.18f ? bar_color : quiet_color);
                g.drawRoundedRect(QRectF(x, center_y - bar_height / 2.0, bar_width, bar_height),
                                  bar_width / 2.0, bar_width / 2.0);
            }

            // Listening is shown when recording starts; keep the version in its footer.
            QFont version_font = this->font();
            version_font.setPixelSize(12);
            g.setFont(version_font);
            g.setPen(with_alpha(COLOR_TEXT, 190));
            g.drawText(QRectF(PADDING, OVERLAY_HEIGHT - PADDING - VERSION_TEXT_HEIGHT,
                              OVERLAY_WIDTH - PADDING * 2, VERSION_TEXT_HEIGHT),
                       Qt::AlignLeft | Qt::AlignBottom,
                       QStringLiteral("V" VTT_VERSION_STRING));
        }
    }

private:
    void clear_queued_start(const char* reason) {
        if (queued_start_flag) queued_start_flag->store(false);
        if (!queued_start_) return;
        queued_start_ = false;
        vtlog("overlay: queued start mark removed (%s)", reason);
        resize_for_state();
        if (isVisible()) update();
    }

    void resize_for_state() {
        const int height = state_ != OverlayState::Processing ? OVERLAY_HEIGHT
                           : queued_start_ ? OVERLAY_QUEUED_HEIGHT
                                           : OVERLAY_COMPACT_HEIGHT;
        if (this->height() != height) setFixedSize(OVERLAY_WIDTH, height);
    }

    int hit_test(const QPoint& point) const {
        for (int i = 0; i < PASTE_BUTTON_COUNT; ++i) {
            if (button_rects_[i].contains(point)) return i;
        }
        return -1;
    }

    void set_hover(int button) {
        if (button == hover_button_) return;
        hover_button_ = button;
        dismiss_tooltip();
        if (button >= 0) tooltip_timer_.start(TOOLTIP_DELAY_MS);
        update();
    }

    void dismiss_tooltip() {
        tooltip_timer_.stop();
        if (tooltip_.isVisible()) tooltip_.hide();
    }

    // Only the mode buttons take the pointer, and only while listening;
    // everywhere else clicks go to the window underneath.
    void update_input_region() {
        if (!windowHandle()) return;
        QRegion region;
        if (state_ == OverlayState::Listening) {
            for (const QRect& rect : button_rects_) region += rect;
        } else {
            region = QRegion(-8, -8, 1, 1); // nothing inside the surface
        }
        windowHandle()->setMask(region);
    }

    static void draw_fallback_icon(QPainter& g, int index, const QRectF& rc,
                                   const QColor& color) {
        g.setPen(QPen(color, 1.7));
        g.setBrush(Qt::NoBrush);
        if (index == 0) { // microphone
            g.drawRoundedRect(QRectF(rc.x() + 14, rc.y() + 4, rc.width() - 28, rc.height() - 18),
                              7, 7);
            g.drawArc(QRectF(rc.x() + 9, rc.y() + 10, rc.width() - 18, rc.height() - 18),
                      180 * 16, 180 * 16);
            g.drawLine(QPointF(rc.center().x(), rc.bottom() - 8),
                       QPointF(rc.center().x(), rc.bottom() - 3));
        } else if (index == 1) { // robot
            const QRectF head(rc.x() + 8, rc.y() + 12, rc.width() - 16, rc.height() - 20);
            g.drawRoundedRect(head, 4, 4);
            g.setBrush(color);
            g.drawEllipse(QPointF(head.x() + 8, head.y() + 8), 2, 2);
            g.drawEllipse(QPointF(head.right() - 8, head.y() + 8), 2, 2);
            g.drawLine(QPointF(head.x() + 8, head.bottom() - 6),
                       QPointF(head.right() - 8, head.bottom() - 6));
            g.drawLine(QPointF(rc.center().x(), rc.y() + 12), QPointF(rc.center().x(), rc.y() + 6));
        } else { // page with lines
            const QRectF page(rc.x() + 10, rc.y() + 6, rc.width() - 20, rc.height() - 12);
            g.drawRect(page);
            for (int line = 0; line < 3; ++line) {
                g.drawLine(QPointF(page.x() + 5, page.y() + 8 + line * 7),
                           QPointF(page.right() - 5 - line * 4, page.y() + 8 + line * 7));
            }
        }
    }

    OverlayState state_ = OverlayState::Hidden;
    QString text_;
    std::string paste_output_mode_ = "clean";
    float audio_level_ = 0.0f;
    float progress_ = 0.0f;
    std::vector<float> audio_levels_;
    std::array<QRect, PASTE_BUTTON_COUNT> button_rects_;
    std::array<QImage, PASTE_BUTTON_COUNT> icons_;
    std::array<QImage, PASTE_BUTTON_COUNT> faded_icons_;
    int hover_button_ = -1;
    bool queued_start_ = false;
    QElapsedTimer clock_;
    QTimer tooltip_timer_;
    TooltipWidget tooltip_;
    bool layer_shell_ = false;
};

} // namespace

// Lives on the UI thread; kept alive by the closures queued to that thread.
struct OverlayWindow::State {
    OverlayWidget* widget = nullptr;
    // Remembered so they can be set before the window exists.
    std::string paste_output_mode = "clean";
    std::function<void(const std::string&)> callback;
    // Set and cleared by the widget on the UI thread; read by the app core.
    std::atomic<bool> queued_start_shown{false};
};

OverlayWindow::OverlayWindow() : state_(std::make_shared<State>()) {}

OverlayWindow::~OverlayWindow() {
    shutdown();
}

bool OverlayWindow::initialize(HINSTANCE /*hInstance*/) {
    if (!state_->widget) {
        state_->widget = new OverlayWidget();
        state_->widget->set_paste_output_mode(state_->paste_output_mode);
        state_->widget->on_mode_selected = state_->callback;
        state_->widget->queued_start_flag = &state_->queued_start_shown;
    }
    return true;
}

void OverlayWindow::shutdown() {
    ui::run([state = state_]() {
        delete state->widget;
        state->widget = nullptr;
    });
}

void OverlayWindow::show(OverlayState overlay_state, const std::string& text) {
    ui::run([state = state_, overlay_state, text]() {
        if (state->widget) state->widget->present(overlay_state, text);
    });
}

void OverlayWindow::hide() {
    ui::run([state = state_]() {
        if (state->widget) state->widget->dismiss();
    });
}

void OverlayWindow::set_audio_level(float level) {
    ui::run([state = state_, level]() {
        if (state->widget) state->widget->set_audio_level(level);
    });
}

void OverlayWindow::set_progress(float progress, const std::string& text) {
    ui::run([state = state_, progress, text]() {
        if (state->widget) state->widget->set_progress(progress, text);
    });
}

void OverlayWindow::set_status_text(const std::string& text) {
    ui::run([state = state_, text]() {
        if (state->widget) state->widget->set_status_text(text);
    });
}

void OverlayWindow::set_paste_output_mode(const std::string& mode) {
    ui::run([state = state_, mode]() {
        state->paste_output_mode = mode;
        if (state->widget) state->widget->set_paste_output_mode(mode);
    });
}

void OverlayWindow::set_queued_start(bool queued) {
    ui::run([state = state_, queued]() {
        if (state->widget) {
            state->widget->set_queued_start(queued);
        } else if (queued) {
            vtlog("overlay: queued start not shown (overlay turned off)");
        }
    });
}

bool OverlayWindow::queued_start_shown() const {
    return state_->queued_start_shown.load();
}

void OverlayWindow::set_paste_mode_callback(
    std::function<void(const std::string&)> callback) {
    ui::run([state = state_, callback = std::move(callback)]() {
        state->callback = callback;
        if (state->widget) state->widget->on_mode_selected = callback;
    });
}

} // namespace vtt
