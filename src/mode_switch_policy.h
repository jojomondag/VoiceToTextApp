#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace vtt {

// An explicit mode selection applies to the current owned text and subsequent
// speech. Background results cannot change that selection. The owner calls this
// object on one thread and must use a new target_token whenever user input
// invalidates ownership of the external range.
class ModeSwitchPolicy {
public:
    struct Context {
        std::uint64_t session = 0;
        std::uint64_t target_token = 0;
        std::string raw_text;
    };

    struct Versions {
        Context source;
        std::string clean;
        std::string ai;
        std::string clean_error;
        std::string ai_error;
    };

    struct Replacement {
        std::uint64_t click_id = 0;
        Context context;
        std::string mode;
        std::string text;
        std::string error;
    };

    // Appended speech keeps a pending prefix eligible. A different target,
    // session, or a change to existing raw text invalidates old work while
    // retaining the selected mode for the new owned source.
    void observe_context(Context context);

    // Returns zero for a stale context or unknown mode. A valid selection also
    // works before there is any owned text. Its id remains stable as speech
    // grows. Every valid call supersedes the earlier selection and may retry a
    // failed source. Modes are "recorded", "clean", and "ai".
    std::uint64_t request_mode(const Context& context, const std::string& mode);

    // Caches results without changing the selection. A result can complete
    // pending work only for its exact source and the selected mode.
    void receive_versions(Versions versions);

    // Consumes at most one replacement for the selected mode, then schedules
    // newer speech if present. Its context.raw_text is the frozen request
    // prefix. Recorded mode restores the current raw text only once per click.
    std::optional<Replacement> take_ready();
    // Unsubmitted speech coalesces into the latest candidate. Mark the exact
    // candidate submitted only when dispatching it; that prefix then freezes.
    std::optional<Context> pending_context() const;
    // Automatic AI work includes completed sentences only; an unfinished tail
    // remains raw in the editor. Explicit clicks/Stop flush the whole source.
    // Clean mode retains phrase-sized batching. The returned prefix is exact.
    std::optional<Context> submission_context(bool flush = false) const;
    bool ready_to_submit(bool flush = false) const;
    bool mark_submitted(const Context& context);
    // True while there is waiting or ready-but-unconsumed work.
    bool has_pending() const;

    void revoke();
    void clear();

private:
    struct Click {
        std::uint64_t id = 0;
        Context context;
        std::string mode;
    };

    struct Selection {
        std::uint64_t id = 0;
        std::string mode;
    };

    static bool same_target(const Context& left, const Context& right);
    static bool same_source(const Context& left, const Context& right);
    static bool is_prefix(const std::string& prefix, const std::string& text);
    bool source_is_owned(const Context& source) const;
    void schedule_current();
    void resolve_pending();
    void invalidate_work();

    static constexpr std::size_t cache_limit_ = 16;
    std::optional<Context> current_;
    std::optional<Selection> selected_;
    std::optional<Click> pending_;
    bool submitted_ = false;
    std::optional<Replacement> ready_;
    std::optional<Context> completed_;
    std::deque<Versions> cache_;
    std::uint64_t next_click_id_ = 0;
};

} // namespace vtt
