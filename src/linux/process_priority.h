#pragma once

namespace vtt {

// Gives the app's own systemd user unit (the scope a launcher such as
// `uwsm app` puts it in) a higher CPU weight, so speech recognition keeps up
// while other programs load every core. Does nothing unless the unit holds
// this process alone. Runs in the background and logs the outcome.
void raise_own_cpu_weight();

} // namespace vtt
