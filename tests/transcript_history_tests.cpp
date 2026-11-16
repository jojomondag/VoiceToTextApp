// History file on Windows: pending entries, their completion and failure
// count, and the save through a temporary file that replaces the old one.
#include "transcript_history.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

nlohmann::json read_json(const std::filesystem::path& path) {
    return nlohmann::json::parse(read_file(path));
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("vtt-history-test-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// Every save after the first replaces the existing file (on Windows a plain
// rename onto an existing name fails), and the result loads again.
void saves_replace_the_existing_file(const std::filesystem::path& dir) {
    const auto file = dir / "history.json";
    vtt::TranscriptHistory history;
    require(history.initialize(file.string(), 100), "initialize failed");
    const std::string recorded = u8"Första diktatet, åäö.";
    const auto id = history.append_pending(recorded);
    require(id != 0 && std::filesystem::exists(file), "the pending entry must be saved at once");
    auto saved = read_json(file);
    require(saved.size() == 1 && saved[0].at("llm_pending") == true && saved[0].at("recorded") == recorded,
            "first save: one pending entry");

    require(history.mark_failed(id, 20) == 1, "first failure must count 1");
    saved = read_json(file);
    require(saved[0].at("llm_attempts") == 1 && saved[0].at("llm_pending") == true,
            "second save must replace the file (attempt count 1)");

    require(history.complete(id, recorded, u8"Städat.", u8"AI-text.", "general"), "complete failed");
    saved = read_json(file);
    require(saved.size() == 1 && !saved[0].contains("llm_pending") && saved[0].at("transcribed") == u8"Städat.",
            "third save must replace the file with the completed entry");

    for (int i = 0; i < 5; ++i) history.append(u8"Mer " + std::to_string(i), "x", "", "general");
    saved = read_json(file);
    require(saved.size() == 6, "later saves must keep replacing the file");
    require(!std::filesystem::exists(file.string() + ".tmp"), "no temporary file may be left behind");

    vtt::TranscriptHistory reloaded;
    require(reloaded.initialize(file.string(), 100), "reload failed");
    const auto entries = reloaded.entries();
    require(entries.size() == 6 && entries[0].id == id && !entries[0].llm_pending &&
            entries[0].transcribed == u8"Städat.", "the replaced file must load with the same entries");
    // A new pending entry after a reload gets a fresh id.
    require(reloaded.append_pending("Nytt.") == id + 1, "ids must continue after a reload");
    std::cout << "PASS: second and later saves replace the existing history file ("
              << read_json(file).size() << " entries after reload)\n";
}

// A save that cannot finish leaves the previous file as it was.
void failed_save_keeps_the_old_file(const std::filesystem::path& dir) {
    const auto file = dir / "locked.json";
    vtt::TranscriptHistory history;
    require(history.initialize(file.string(), 100), "initialize failed");
    history.append("Gammal post.", "Gammal post.", "", "general");
    const std::string before = read_file(file);
    require(!before.empty(), "the first save must exist");

    // 1. The target is open without FILE_SHARE_DELETE (as an editor, a
    //    virus scanner or a backup tool may hold it): it cannot be replaced.
    HANDLE holder = CreateFileW(file.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(holder != INVALID_HANDLE_VALUE, "could not open the history file");
    history.append("Ny post medan filen var låst.", "x", "", "general");
    CloseHandle(holder);
    const std::string while_locked = read_file(file);
    require(while_locked == before, "a save onto a locked file must leave the old file intact");
    require(!std::filesystem::exists(file.string() + ".tmp"), "a failed save must remove its temporary file");

    // 2. The temporary file cannot be written.
    std::filesystem::create_directories(file.string() + ".tmp");
    history.append("Ytterligare en post.", "x", "", "general");
    require(read_file(file) == before, "a save without a temporary file must leave the old file intact");
    std::filesystem::remove(file.string() + ".tmp");

    // The next save that can finish writes everything kept in memory.
    history.append("Sista posten.", "x", "", "general");
    require(read_json(file).size() == 4, "the next successful save must write every entry");
    std::cout << "PASS: failed saves (locked target, unwritable temp file) leave the old file intact\n";
}

} // namespace

int main() {
    try {
        TempDir dir;
        saves_replace_the_existing_file(dir.path);
        failed_save_keeps_the_old_file(dir.path);
        std::cout << "Transcript history tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Transcript history test failed: " << error.what() << '\n';
        return 1;
    }
}
