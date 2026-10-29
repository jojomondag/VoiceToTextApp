#include "mode_switch_policy.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using Policy = vtt::ModeSwitchPolicy;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Policy::Context context(std::string raw, std::uint64_t token = 10,
                        std::uint64_t session = 1) {
    return {session, token, std::move(raw)};
}

Policy::Versions version(const Policy::Context& source, const std::string& mode,
                         const std::string& text, const std::string& error = {}) {
    Policy::Versions result;
    result.source = source;
    if (mode == "clean") {
        result.clean = text;
        result.clean_error = error;
    } else {
        result.ai = text;
        result.ai_error = error;
    }
    return result;
}

void require_pending(const Policy& policy, const Policy::Context& expected) {
    const auto pending = policy.pending_context();
    require(policy.has_pending() && pending.has_value(), "expected pending source");
    require(pending->session == expected.session &&
            pending->target_token == expected.target_token &&
            pending->raw_text == expected.raw_text, "wrong pending source");
}

void selection_before_speech(const std::string& mode) {
    Policy policy;
    const auto empty = context("", 0);
    policy.observe_context(empty);
    const auto id = policy.request_mode(empty, mode);
    require(id != 0, "empty selection must be accepted");
    require(!policy.has_pending() && !policy.take_ready(), "empty source cannot run");

    const auto unowned = context("unowned text", 0);
    policy.observe_context(unowned);
    policy.receive_versions(version(unowned, mode, "do not apply"));
    require(!policy.has_pending() && !policy.take_ready(), "zero token cannot run");

    const auto owned = context("first words");
    policy.observe_context(owned);
    require_pending(policy, owned);
    policy.receive_versions(version(owned, mode, "First words."));
    const auto ready = policy.take_ready();
    require(ready && ready->click_id == id && ready->mode == mode &&
            ready->text == "First words.", "empty selection did not persist");

    Policy with_empty_range;
    const auto empty_owned = context("");
    with_empty_range.observe_context(empty_owned);
    require(with_empty_range.request_mode(empty_owned, mode) != 0,
            "empty owned selection must be accepted");
    with_empty_range.observe_context(owned);
    require_pending(with_empty_range, owned);
}

void transforms_current_and_continued_speech(const std::string& mode) {
    Policy policy;
    const auto first = context("one");
    const auto second = context("one two");
    const auto third = context("one two three");
    policy.observe_context(first);
    const auto id = policy.request_mode(first, mode);
    require_pending(policy, first);
    require(policy.mark_submitted(first), "submitted prefix could not be frozen");

    policy.observe_context(second);
    require_pending(policy, first);
    // Newer speculative output must not skip or replace the frozen prefix.
    policy.receive_versions(version(second, mode, "Second."));
    require_pending(policy, first);
    require(!policy.take_ready(), "newer result consumed before frozen source");
    policy.receive_versions(version(first, mode, "First."));
    const auto ready_first = policy.take_ready();
    require(ready_first && ready_first->click_id == id &&
            ready_first->context.raw_text == first.raw_text &&
            ready_first->text == "First.", "initial source was not transformed");
    require(policy.has_pending() && !policy.pending_context(),
            "cached ready result must keep final drain active");
    const auto ready_second = policy.take_ready();
    require(ready_second && ready_second->click_id == id &&
            ready_second->context.raw_text == second.raw_text &&
            ready_second->text == "Second.", "continued speech was not transformed");
    require(!policy.has_pending() && !policy.take_ready(), "source was rewritten twice");

    policy.receive_versions(version(first, mode, "Obsolete."));
    policy.receive_versions(version(second, mode, "Duplicate."));
    policy.observe_context(second);
    require(!policy.has_pending() && !policy.take_ready(), "late result caused a rewrite");

    policy.observe_context(third);
    require_pending(policy, third);
    policy.receive_versions(version(first, mode, "Older shorter source."));
    require(!policy.take_ready(), "older source displaced newer pending work");
    policy.receive_versions(version(third, mode, "Third."));
    const auto ready_third = policy.take_ready();
    require(ready_third && ready_third->click_id == id && ready_third->text == "Third.",
            "selection id did not survive continued speech");
}

void mode_switch_supersedes_old_results() {
    Policy policy;
    const auto source = context("current text");
    policy.observe_context(source);
    const auto clean_id = policy.request_mode(source, "clean");
    policy.receive_versions(version(source, "clean", "Clean."));
    const auto ai_id = policy.request_mode(source, "ai");
    require(ai_id != clean_id, "mode switch needs a new selection id");
    require_pending(policy, source);
    require(!policy.take_ready(), "old ready mode survived selection change");
    policy.receive_versions(version(source, "clean", "Updated clean."));
    require(!policy.take_ready(), "background result changed selected mode");
    policy.receive_versions(version(source, "ai", "Translated."));
    const auto ai = policy.take_ready();
    require(ai && ai->click_id == ai_id && ai->mode == "ai" &&
            ai->text == "Translated.", "selected AI result was not applied");

    const auto next_clean_id = policy.request_mode(source, "clean");
    const auto clean = policy.take_ready();
    require(clean && clean->click_id == next_clean_id &&
            clean->text == "Updated clean.", "successful mode cache was lost");
    const auto retry_id = policy.request_mode(source, "clean");
    const auto retry = policy.take_ready();
    require(retry && retry->click_id == retry_id && retry_id != next_clean_id,
            "same-mode selection did not supersede completed work");
}

void ownership_changes_retain_selection() {
    Policy policy;
    auto old = context("original");
    policy.observe_context(old);
    const auto id = policy.request_mode(old, "ai");
    const Policy::Context changes[] = {
        context("new target", 11),
        context("new session", 11, 2),
        context("manual replacement", 11, 2)
    };
    for (const auto& next : changes) {
        // Invalidate already completed background work that was not consumed.
        policy.receive_versions(version(old, "ai", "Obsolete ready text."));
        policy.observe_context(next);
        require_pending(policy, next);
        policy.receive_versions(version(old, "ai", "Obsolete late text."));
        require(!policy.take_ready(), "invalid ownership result survived");
        policy.receive_versions(version(next, "ai", "Current result."));
        const auto ready = policy.take_ready();
        require(ready && ready->click_id == id &&
                ready->context.raw_text == next.raw_text,
                "selected mode was lost after ownership change");
        old = next;
    }
    policy.observe_context(context("", 0, 2));
    require(!policy.has_pending() && !policy.take_ready(), "unowned reset has work");
    const auto resumed = context("new dictated span", 12, 2);
    policy.observe_context(resumed);
    require_pending(policy, resumed);
}

void recorded_restores_only_once() {
    Policy policy;
    const auto first = context("raw words");
    const auto second = context("raw words and more");
    policy.observe_context(first);
    policy.request_mode(first, "ai");
    policy.receive_versions(version(first, "ai", "Translated."));
    const auto id = policy.request_mode(first, "recorded");
    policy.observe_context(second);
    const auto ready = policy.take_ready();
    require(ready && ready->click_id == id && ready->mode == "recorded" &&
            ready->text == first.raw_text, "recorded did not restore clicked raw prefix");
    require(!policy.has_pending() && !policy.take_ready(), "recorded rewrote appended speech");
    policy.receive_versions(version(second, "ai", "Stale AI."));
    policy.observe_context(context("different target", 20));
    require(!policy.has_pending() && !policy.take_ready(), "recorded persisted as a rewrite");

    Policy empty_policy;
    const auto empty = context("", 0);
    empty_policy.observe_context(empty);
    require(empty_policy.request_mode(empty, "recorded") != 0,
            "recorded selection before speech rejected");
    empty_policy.observe_context(first);
    require(!empty_policy.has_pending() && !empty_policy.take_ready(),
            "recorded before speech scheduled unnecessary rewrite");
}

void ownership_can_be_regained_for_same_source() {
    Policy policy;
    const auto source = context("same raw text");
    policy.observe_context(source);
    const auto id = policy.request_mode(source, "clean");
    policy.receive_versions(version(source, "clean", "Before reset."));
    require(policy.take_ready().has_value(), "initial result missing");

    policy.observe_context(context(source.raw_text, 0));
    require(!policy.has_pending(), "unowned source scheduled work");
    policy.receive_versions(version(source, "clean", "Late stale result."));
    policy.observe_context(source);
    require_pending(policy, source);
    require(!policy.take_ready(), "regained ownership reused invalidated result");
    policy.receive_versions(version(source, "clean", "After reset."));
    const auto ready = policy.take_ready();
    require(ready && ready->click_id == id && ready->text == "After reset.",
            "same source could not regain selected mode");
}

void failures_complete_once_and_allow_retry(const std::string& mode) {
    Policy policy;
    const auto first = context("one");
    const auto second = context("one two");
    policy.observe_context(first);
    const auto id = policy.request_mode(first, mode);
    policy.receive_versions(version(first, mode, "", "network error"));
    const auto failed = policy.take_ready();
    require(failed && failed->click_id == id && failed->error == "network error" &&
            failed->text == first.raw_text, "error did not preserve raw source");
    policy.observe_context(first);
    policy.receive_versions(version(first, mode, "", "network error"));
    require(!policy.has_pending() && !policy.take_ready(), "failed source retried continuously");

    const auto retry_id = policy.request_mode(first, mode);
    require(retry_id != id, "explicit retry retained old selection id");
    require_pending(policy, first);
    require(!policy.take_ready(), "explicit retry replayed cached failure");
    require(policy.mark_submitted(first), "retry source could not be frozen");
    policy.observe_context(second);
    policy.receive_versions(version(first, mode, "", "still offline"));
    const auto retried = policy.take_ready();
    require(retried && retried->click_id == retry_id && !retried->error.empty(),
            "retry did not report failure");
    require_pending(policy, second);
    policy.receive_versions(version(second, mode, "Recovered."));
    const auto recovered = policy.take_ready();
    require(recovered && recovered->click_id == retry_id &&
            recovered->text == "Recovered.", "newer speech did not recover from failure");
}

void invalid_requests_and_reset() {
    Policy policy;
    const auto source = context("words");
    require(policy.request_mode(source, "clean") == 0, "unobserved context accepted");
    policy.observe_context(source);
    require(policy.request_mode(source, "unknown") == 0, "unknown mode accepted");
    policy.request_mode(source, "clean");
    require(policy.request_mode(context("stale"), "ai") == 0, "stale context accepted");
    require_pending(policy, source);
    policy.clear();
    policy.observe_context(source);
    policy.receive_versions(version(source, "clean", "Background only."));
    require(!policy.has_pending() && !policy.take_ready(), "clear retained mode selection");
    policy.request_mode(source, "ai");
    policy.revoke();
    policy.observe_context(context("next source", 20));
    require(!policy.has_pending() && !policy.take_ready(), "revoke retained mode selection");
}

void cache_is_bounded() {
    Policy policy;
    const auto source = context(std::string(80, 'a'));
    policy.observe_context(source);
    policy.receive_versions(version(source, "clean", "Oldest cache entry."));
    for (std::size_t size = 1; size <= 64; ++size) {
        policy.receive_versions(version(context(std::string(size, 'a')), "clean", "Prefix."));
    }
    require(!policy.take_ready(), "cache entry independently authorized replacement");
    policy.request_mode(source, "clean");
    require_pending(policy, source);
    require(!policy.take_ready(), "cache retained an unbounded old entry");
    policy.receive_versions(version(source, "clean", "Fresh."));
    const auto ready = policy.take_ready();
    require(ready && ready->text == "Fresh.", "evicted source could not be regenerated");
}

void fragments_coalesce_before_submission_and_keep_whole_context() {
    Policy policy;
    const auto empty = context("", 0);
    policy.observe_context(empty);
    const auto id = policy.request_mode(empty, "ai");
    const auto fragment = context("Jag ");
    policy.observe_context(fragment);
    require_pending(policy, fragment);
    require(!policy.ready_to_submit(), "isolated four-byte fragment reached the model");
    require(policy.ready_to_submit(true), "explicit click or stop could not flush a short source");
    const auto phrase = context("Jag behover bygga en implementation av Teachers Little Helper.");
    policy.observe_context(phrase);
    require_pending(policy, phrase);
    require(policy.ready_to_submit(), "complete accumulated sentence was not ready");
    require(!policy.mark_submitted(fragment), "stale unsubmitted fragment was frozen");
    require(policy.mark_submitted(phrase), "current accumulated phrase could not be submitted");
    require(!policy.ready_to_submit() && !policy.ready_to_submit(true),
        "an already submitted prefix was submitted twice");

    const auto continued = context(phrase.raw_text + " Den ska ratta uppgifter fran Google Classroom.");
    policy.observe_context(continued);
    require_pending(policy, phrase);
    require(!policy.mark_submitted(continued), "new speech displaced an in-flight request");
    policy.receive_versions(version(phrase, "ai", "Implementera Teachers Little Helper."));
    const auto first = policy.take_ready();
    require(first && first->click_id == id && first->context.raw_text == phrase.raw_text,
        "frozen accumulated source was not resolved");
    require_pending(policy, continued);
    require(policy.ready_to_submit(), "substantial new speech was not scheduled");
    require(policy.mark_submitted(continued), "complete growing source was not frozen");
    policy.receive_versions(version(continued, "ai", "Implementera rattning i Teachers Little Helper via Google Classroom."));
    const auto second = policy.take_ready();
    require(second && second->context.raw_text == continued.raw_text && second->click_id == id,
        "continued request lost the previous original speech");

    const auto short_tail = context(continued.raw_text + " nu");
    policy.observe_context(short_tail);
    require_pending(policy, short_tail);
    require(!policy.ready_to_submit(), "long previous context incorrectly made a tiny tail ready");
    require(policy.ready_to_submit(true) && policy.mark_submitted(short_tail),
        "Stop did not flush the final short tail with its whole original context");
    policy.receive_versions(version(short_tail, "ai", "Implementera rattningen nu."));
    const auto final = policy.take_ready();
    require(final && final->context.raw_text == short_tail.raw_text && !policy.has_pending(),
        "final tiny suffix was omitted or requested without prior context");
}

void ai_waits_for_completion_and_stop_preserves_unresolved_fragments() {
    Policy policy;
    const auto unfinished = context(u8"Vi behöver lägga till flera möjligheter att kunna anpassa applikationen till");
    policy.observe_context(unfinished);
    policy.request_mode(unfinished, "ai");
    require_pending(policy, unfinished);
    require(!policy.submission_context() && !policy.ready_to_submit(),
        "unfinished live 'till' prefix reached AI and could generate premature clarification");
    const auto stopped = policy.submission_context(true);
    require(stopped && stopped->raw_text == unfinished.raw_text && policy.mark_submitted(*stopped),
        "Stop could not submit the complete unresolved original source");
    const std::string clarification = u8"Behöver förtydligas: \"" + unfinished.raw_text + "\"";
    policy.receive_versions(version(*stopped, "ai", clarification));
    const auto result = policy.take_ready();
    require(result && result->text == clarification && result->context.raw_text == unfinished.raw_text &&
        !policy.has_pending(), "final unresolved source lost its clarification or remained pending");
}

void ai_freezes_completed_prefix_without_consuming_new_speech() {
    Policy policy;
    const std::string first_sentence = u8"Anpassa applikationen till olika språk.";
    const auto initial = context(first_sentence + u8" Det ska gå att");
    policy.observe_context(initial);
    policy.request_mode(initial, "ai");
    require_pending(policy, initial);
    const auto prefix = policy.submission_context();
    require(prefix && prefix->raw_text == first_sentence &&
        initial.raw_text.substr(prefix->raw_text.size()) == u8" Det ska gå att",
        "automatic candidate consumed or changed the unfinished raw tail");
    require(!policy.mark_submitted(context(u8"Anpassa applikationen")),
        "an arbitrary shorter fragment was accepted instead of the eligible exact prefix");
    require(policy.mark_submitted(*prefix), "complete prefix could not be frozen");

    const auto growing = context(initial.raw_text + u8" välja vilket språk");
    policy.observe_context(growing);
    require_pending(policy, *prefix);
    policy.receive_versions(version(*prefix, "ai", u8"Lägg till stöd för olika språk."));
    const auto first = policy.take_ready();
    require(first && first->context.raw_text == first_sentence,
        "completed result replaced more than its frozen original sentence");
    require_pending(policy, growing);
    require(!policy.submission_context() && !policy.mark_submitted(*prefix),
        "already completed sentence was repeatedly submitted while its tail was unfinished");
    const auto flushed = policy.submission_context(true);
    require(flushed && flushed->raw_text == growing.raw_text,
        "Stop candidate omitted the unfinished tail or previous original context");

    const auto complete = context(growing.raw_text + u8" texten skrivs på.");
    policy.observe_context(complete);
    const auto next = policy.submission_context();
    require(next && next->raw_text == complete.raw_text && policy.mark_submitted(*next),
        "finishing the next sentence did not submit all original context exactly once");
    policy.receive_versions(version(*next, "ai", u8"Lägg till språkstöd och låt användaren välja textspråk."));
    require(policy.take_ready().has_value() && !policy.submission_context() && !policy.has_pending(),
        "completed growing source was lost or scheduled twice");
}

void ai_sentence_boundaries_ignore_technical_dots_and_ellipses() {
    for (const std::string raw : {
        "Anvand version 1.2 och oppna config.json", "Anvand vardet 3.14 i app.cpp",
        "Vi behover anpassa applikationen till...", u8"Vi behöver anpassa applikationen till…"}) {
        Policy policy;
        const auto source = context(raw);
        policy.observe_context(source);
        policy.request_mode(source, "ai");
        require(!policy.submission_context() && policy.submission_context(true).has_value(),
            "decimal, file extension, version or ellipsis was treated as a completed sentence");
    }
    Policy policy;
    const std::string completed = u8"Skriv texten \"Gör så.\"";
    const auto source = context(completed + u8" Sedan ska vi");
    policy.observe_context(source);
    policy.request_mode(source, "ai");
    const auto prefix = policy.submission_context();
    require(prefix && prefix->raw_text == completed,
        "sentence prefix omitted its closing quote or consumed the unfinished next sentence");
}

void clean_retains_phrase_batching_without_sentence_punctuation() {
    Policy policy;
    const auto source = context("Jag behover bygga en implementation av Teachers Little Helper");
    policy.observe_context(source);
    policy.request_mode(source, "clean");
    const auto candidate = policy.submission_context();
    require(candidate && candidate->raw_text == source.raw_text && policy.mark_submitted(*candidate),
        "clean mode no longer processes an accumulated unpunctuated phrase");
    policy.receive_versions(version(*candidate, "clean", "Bygg en implementation av Teachers Little Helper."));
    require(policy.take_ready().has_value(), "clean phrase result missing");
    const auto short_tail = context(source.raw_text + " nu");
    policy.observe_context(short_tail);
    require(!policy.submission_context() && policy.submission_context(true)->raw_text == short_tail.raw_text,
        "clean mode lost its short-tail batching or explicit flush behavior");
}

void short_complete_sentences_and_explicit_flush_are_ready() {
    Policy policy;
    const auto source = context("Skriv kort.");
    policy.observe_context(source);
    policy.request_mode(source, "clean");
    require(policy.ready_to_submit(), "short complete sentence waited for a size threshold");
    require(policy.mark_submitted(source), "short complete sentence could not be frozen");
    policy.receive_versions(version(source, "clean", "Skriv kort."));
    require(policy.take_ready().has_value(), "complete sentence was not consumed");
    const auto tail = context("Skriv kort. Tack.");
    policy.observe_context(tail);
    require(policy.ready_to_submit(), "complete short tail did not retain its previous sentence as context");
    const auto isolated = context("Tack.", 22);
    policy.observe_context(isolated);
    require(!policy.ready_to_submit(), "isolated single word was sent as an instruction");
    require(policy.ready_to_submit(true), "Stop could not process a single final word");
}

} // namespace

int main() {
    try {
        for (const std::string mode : {"clean", "ai"}) {
            selection_before_speech(mode);
            transforms_current_and_continued_speech(mode);
            failures_complete_once_and_allow_retry(mode);
        }
        mode_switch_supersedes_old_results();
        ownership_changes_retain_selection();
        ownership_can_be_regained_for_same_source();
        recorded_restores_only_once();
        invalid_requests_and_reset();
        cache_is_bounded();
        fragments_coalesce_before_submission_and_keep_whole_context();
        ai_waits_for_completion_and_stop_preserves_unresolved_fragments();
        ai_freezes_completed_prefix_without_consuming_new_speech();
        ai_sentence_boundaries_ignore_technical_dots_and_ellipses();
        clean_retains_phrase_batching_without_sentence_punctuation();
        short_complete_sentences_and_explicit_flush_are_ready();
        std::cout << "Mode switch policy tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mode switch policy test failed: " << error.what() << '\n';
        return 1;
    }
}
