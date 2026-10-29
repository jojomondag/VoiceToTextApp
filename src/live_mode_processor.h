#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

namespace vtt {

// Serializes local language-model work without blocking microphone or UI events.
// The owner must keep the processor alive until shutdown has returned and must
// not destroy it from a callback. Callbacks may submit or invalidate requests.
class LiveModeProcessor {
public:
    struct Request {
        std::uint64_t session = 0;
        std::uint64_t mode_generation = 0;
        std::uint64_t target_token = 0;
        std::string raw_text;
        std::string mode;
        bool final = false;
        bool urgent = false;
        bool append_only = false;
        int utterance = -1;
    };

    struct Result {
        Request request;
        std::string text;
        std::string error;
        std::string cleaned_text;
        std::string ai_text;
        std::string clean_error;
        std::string ai_error;
    };

    struct Timing {
        std::chrono::milliseconds debounce{450};
        std::chrono::milliseconds max_wait{1200};
    };

    using Transform = std::function<Result(const Request&)>;
    using Publish = std::function<void(Result)>;

    LiveModeProcessor(Transform transform, Publish publish);
    LiveModeProcessor(Transform transform, Publish publish, Timing timing);
    ~LiveModeProcessor();

    LiveModeProcessor(const LiveModeProcessor&) = delete;
    LiveModeProcessor& operator=(const LiveModeProcessor&) = delete;

    // Invalidates pending/in-flight results even when the identifiers repeat.
    // Call this on session/mode changes, including a change back to raw speech.
    void set_generation(std::uint64_t session, std::uint64_t mode_generation);
    // Only requests matching the current generation are accepted. A changed
    // target_token invalidates work for the previously focused editable range.
    // append_only requests must be final and preserve distinct utterances in
    // FIFO order. A nonnegative utterance id is deduplicated for this context.
    void submit(Request request);
    // Invalidates work but keeps the current generation available for new work.
    void cancel();
    // Waits for the current transform. No callback runs after this returns.
    // If called in a callback, stops immediately; a later owner-thread shutdown
    // (or the destructor) joins the worker.
    void shutdown();

private:
    using Clock = std::chrono::steady_clock;
    void run();
    void invalidate_locked();

    Transform transform_;
    Publish publish_;
    Timing timing_;
    // Publication is serialized with invalidation. Recursive locking permits a
    // callback to submit/cancel without exposing stale callbacks to the owner.
    std::recursive_mutex mutex_;
    std::condition_variable_any changed_;
    std::mutex join_mutex_;
    std::optional<Request> pending_;
    std::deque<Request> append_pending_;
    std::unordered_set<int> append_utterances_;
    Clock::time_point first_pending_{};
    Clock::time_point due_{};
    bool pending_urgent_ = false;
    bool stopped_ = false;
    std::uint64_t session_ = 0;
    std::uint64_t mode_generation_ = 0;
    std::uint64_t target_token_ = 0;
    std::uint64_t invalidation_ = 0;
    std::thread worker_;
    std::thread::id worker_id_;
};

} // namespace vtt
