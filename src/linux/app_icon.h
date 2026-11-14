#pragma once

#include <QColor>
#include <QIcon>

namespace vtt {

// The app icon: a white "V" on a coloured tile (blue when idle, red while
// recording), drawn at the sizes panels and window lists ask for.
QIcon create_app_icon(const QColor& background);

} // namespace vtt
