#include "live_mode_processor.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vtt {

LiveModeProcessor::LiveModeProcessor(Transform transform, Publish publish)
    : LiveModeProcessor(std::move(transform), std::move(publish), Timing{}) {}

LiveModeProcessor::LiveModeProcessor(Transform transform, Publish publish, Timing timing)
    : transform_(std::move(transform)), publish_(std::move(publish)), timing_(timing) {
    if (!transform_ || !publish_) {
        throw std::invalid_argument("LiveModeProcessor requires transform and publish callbacks");
    }
    timing_.debounce = std::max(timing_.debounce, std::chrono::milliseconds{0});
    timing_.max_wait = std::max(timing_.max_wait, std::chrono::milliseconds{0});
    worker_ = std::thread([this] { run(); });
    worker_id_ = worker_.get_id();
}

LiveModeProcessor::~LiveModeProcessor() {
    shutdown();
}

void LiveModeProcessor::invalidate_locked() {
    ++invalidation_;
    pending_.reset();
    append_pending_.clear();
    append_utterances_.clear();
    pending_urgent_ = false;
}

void LiveModeProcessor::set_generation(std::uint64_t session,
                                       std::uint64_t mode_generation) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (stopped_) return;
    invalidate_locked();
    session_ = session;
    mode_generation_ = mode_generation;
    target_token_ = 0;
    changed_.notify_all();
}

void LiveModeProcessor::submit(Request request) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (stopped_ || request.session != session_ ||
        request.mode_generation != mode_generation_ ||
        (request.append_only && !request.final)) {
        return;
    }
    if (request.target_token != target_token_) {
        invalidate_locked();
        target_token_ = request.target_token;
    }

    if (request.append_only) {
        if (request.utterance >= 0 &&
            !append_utterances_.insert(request.utterance).second) {
            return;
        }
        append_pending_.push_back(std::move(request));
        changed_.notify_all();
        return;
    }

    const auto now = Clock::now();
    if (!pending_) {
        first_pending_ = now;
        pending_urgent_ = false;
    }
    pending_urgent_ = pending_urgent_ || request.final || request.urgent;
    due_ = pending_urgent_ ? now
                          : std::min(now + timing_.debounce,
                                     first_pending_ + timing_.max_wait);
    pending_ = std::move(request);
    changed_.notify_all();
}

void LiveModeProcessor::cancel() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (stopped_) return;
    invalidate_locked();
    changed_.notify_all();
}

void LiveModeProcessor::shutdown() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        stopped_ = true;
        invalidate_locked();
        changed_.notify_all();
    }
    // Do not hold join_mutex_ in a callback: another owner thread may already
    // hold it while waiting for this worker to leave that callback.
    if (std::this_thread::get_id() == worker_id_) return;
    std::lock_guard<std::mutex> joining(join_mutex_);
    if (worker_.joinable()) worker_.join();
}

void LiveModeProcessor::run() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    while (!stopped_) {
        changed_.wait(lock, [this] {
            return stopped_ || pending_.has_value() || !append_pending_.empty();
        });
        if (stopped_) break;
        while (!stopped_ && append_pending_.empty() && pending_ && Clock::now() < due_) {
            changed_.wait_until(lock, due_);
        }
        if (stopped_) break;
        Request request;
        if (!append_pending_.empty()) {
            request = std::move(append_pending_.front());
            append_pending_.pop_front();
        } else if (pending_) {
            request = std::move(*pending_);
            pending_.reset();
            pending_urgent_ = false;
        } else {
            continue;
        }
        const auto invalidation = invalidation_;
        lock.unlock();

        Result result;
        try {
            result = transform_(request);
            // Metadata always belongs to the actual input, regardless of how
            // the application constructs its transform result.
            result.request = request;
            if (result.text.empty() && !result.error.empty()) {
                result.text = request.raw_text;
            }
        } catch (const std::exception& error) {
            result = Result{request, request.raw_text, error.what()};
        } catch (...) {
            result = Result{request, request.raw_text, "Unknown live text processing error"};
        }

        lock.lock();
        // New text in the same range does not stale a completed prefix. The UI
        // checks ownership and can append the newer raw suffix safely itself.
        if (stopped_ || invalidation != invalidation_ ||
            request.session != session_ ||
            request.mode_generation != mode_generation_ ||
            request.target_token != target_token_) {
            continue;
        }
        try {
            publish_(std::move(result));
        } catch (...) {
            // A failed UI dispatch must not terminate the inference worker.
        }
    }
}

} // namespace vtt
