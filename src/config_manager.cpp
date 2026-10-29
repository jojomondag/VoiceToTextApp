#include "config_manager.h"
#include <cctype>
#include <fstream>
#include <filesystem>
#include <set>

namespace vtt {
namespace {

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0);
    if (length <= 0) return {};

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string utf8_from_wide(const std::wstring& text) {
    if (text.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(),
                        static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

std::string windows_1252_from_wide(const std::wstring& text) {
    if (text.empty()) return {};

    BOOL used_default = FALSE;
    int length = WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, &used_default);
    if (length <= 0 || used_default) return {};

    std::string bytes(static_cast<size_t>(length), '\0');
    used_default = FALSE;
    WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, text.data(),
                        static_cast<int>(text.size()), bytes.data(), length,
                        nullptr, &used_default);
    return used_default ? std::string() : bytes;
}

bool looks_like_mojibake(const std::string& text) {
    return text.find("\xC3\x83") != std::string::npos ||
           text.find("\xC3\x82") != std::string::npos ||
           text.find("\xC2\xA5") != std::string::npos;
}

bool repair_mojibake(std::string& text) {
    bool changed = false;

    for (int pass = 0; pass < 3 && looks_like_mojibake(text); ++pass) {
        std::wstring wide = wide_from_utf8(text);
        std::string bytes = windows_1252_from_wide(wide);
        std::wstring repaired_wide = wide_from_utf8(bytes);
        std::string repaired = utf8_from_wide(repaired_wide);

        if (repaired.empty() || repaired == text) break;
        text = repaired;
        changed = true;
    }

    return changed;
}

bool repair_config_text(AppConfig& config) {
    bool changed = false;
    changed |= repair_mojibake(config.llm_pre_prompt);
    changed |= repair_mojibake(config.llm_ai_language_prompt);
    changed |= repair_mojibake(config.llm_intent_detection_prompt);
    changed |= repair_mojibake(config.llm_profile_transform_prompt);
    return changed;
}

bool migrate_ai_language_prompt(AppConfig& config) {
    // Replace only prompts previously shipped by this application. A user's
    // edited prompt is intentional configuration and must remain untouched.
    static const std::string legacy_english =
        "Create an ultra-compact AI-readable instruction dialect from the "
        "user's cleaned instruction. Compress aggressively while preserving "
        "intent, constraints, entities, code names, paths, numbers, and action "
        "order. Remove repetition and filler. Prefer dense structured syntax "
        "with labels such as Goal, Context, Tasks, Constraints, Output. Use "
        "abbreviations only when unambiguous to a coding language model. Do "
        "not add new requirements. Return only the AI-language version.";
    static const std::string legacy_swedish =
        u8"Skapa en ultrakompakt AI-läsbar instruktionsdialekt från användarens "
        u8"städade instruktion. Komprimera aggressivt men bevara avsikt, krav, "
        u8"begränsningar, entiteter, kodnamn, sökvägar, siffror och åtgärdsordning. "
        u8"Lägg inte till nya krav eller antaganden. Svara endast med AI-språkversionen.";
    static const std::string legacy_natural_language =
        "Rewrite the dictated text as a clear, concise instruction in natural "
        "language for an AI assistant. Keep the original language. Remove filler "
        "and repetition, correct obvious transcription errors, and organize "
        "related ideas into complete sentences. Preserve every distinct request, "
        "fact, condition, comparison, exception, negation, and action order. "
        "Keep names and technical terms written out as provided; never invent "
        "abbreviations, identifier codes, symbolic shorthand, or placeholders. "
        "Do not replace information with bracketed labels. Do not invent missing "
        "details or finish an incomplete thought. Do not answer or carry out the "
        "instruction. Return only the rewritten text, without a heading or commentary.";
    static const std::string legacy_one_shot =
        u8"Du redigerar dikterat tal till en färdig prompt för en annan AI. Målet är "
        u8"att AI:n ska förstå användarens avsikt och kunna utföra uppgiften i ett "
        u8"försök, så långt underlaget räcker. Skriv om instruktionen; besvara eller "
        u8"utför den inte. Skriv på diktatets språk, inte språket i dessa regler: "
        u8"svenska in ger svenska ut och engelska in ger engelska ut. Byt bara språk "
        u8"om användaren uttryckligen ber om själva prompten på ett annat språk. En "
        u8"begäran att AI:n ska översätta någon annan text ska förbli en instruktion "
        u8"på diktatets språk. Gör målet tydligt och samla relaterade krav. Använd "
        u8"naturliga meningar och vid behov enkla etiketter med kolon samt numrerade "
        u8"uppgifter för angivet sammanhang, begränsningar och önskat resultat. Korta "
        u8"uppgifter ska förbli korta. Ta bort utfyllnad och upprepningar, rätta "
        u8"entydiga taligenkänningsfel och förtydliga samband som stöds av underlaget. "
        u8"Bevara alla avsikter, fakta, namn, facktermer, siffror, sökvägar, "
        u8"kommandon, identifierare, villkor, jämförelser, undantag, negationer, "
        u8"tidsbegränsningar som ännu och tills vidare och ordningsföljder. Ange "
        u8"uttryckligen vilket utfall varje villkor leder till, inklusive "
        u8"annars-fallet endast när det har angetts. Ovillkorliga krav ska gälla även "
        u8"utanför en villkorsgren. Lägg inte till egna krav, lösningsval eller "
        u8"saknade detaljer. Använd inte påhittade koder, förkortningar, platshållare "
        u8"eller tomma mallfält. En meningsbärande oklar eller avbruten fras ska "
        u8"citeras ordagrant efter etiketten Behöver förtydligas, översatt till "
        u8"diktatets språk. Gissa inte en fortsättning, liknelse, egenskap eller vad "
        u8"frasen syftar på. Frasen ska bara finnas i noteringen, inte samtidigt "
        u8"omvandlas till ett krav. Är allt oklart, skriv bara noteringen med hela "
        u8"diktatet citerat. Exempel: eh gör knappen blå tack -> Gör knappen blå. "
        u8"Exempel: Om filen är tom ska körningen avbrytas, annars sparas resultatet. "
        u8"-> Avbryt körningen om filen är tom. Spara annars resultatet. Exempel: Lägg "
        u8"till export. Exporten ska vara som en fjärde. -> Lägg till export. Behöver "
        u8"förtydligas: ”Exporten ska vara som en fjärde.” Exempel: Översätt ordet "
        u8"dator till spanska. -> Översätt ordet dator till spanska. Example: please "
        u8"make the button green thanks -> Make the button green. Returnera bara den "
        u8"färdiga prompten.";
    static const std::string legacy_plain_formatted =
        u8"Du redigerar dikterat tal till en färdig prompt för en annan AI. Målet är "
        u8"att AI:n ska förstå användarens avsikt och kunna utföra uppgiften i ett "
        u8"försök, så långt underlaget räcker. Skriv om instruktionen; besvara eller "
        u8"utför den inte. Skriv på diktatets språk, inte språket i dessa regler: "
        u8"svenska in ger svenska ut och engelska in ger engelska ut. Byt bara språk "
        u8"om användaren uttryckligen ber om själva prompten på ett annat språk. En "
        u8"begäran att AI:n ska översätta någon annan text ska förbli en instruktion "
        u8"på diktatets språk. Gör målet tydligt och samla relaterade krav. Använd "
        u8"naturliga meningar och en layout som är lätt att läsa för både människor "
        u8"och AI. Vid flera krav: skriv en kort inledning med målet och sedan ett "
        u8"krav per rad i en punktlista. Använd numrerade steg när ordningen spelar "
        u8"roll. Gruppera bara när det behövs, med enkla etiketter följda av kolon "
        u8"för angivet sammanhang, begränsningar eller önskat resultat. Sätt en "
        u8"tomrad mellan stycken och avsnitt. Underpunkter ska bara användas för "
        u8"krav som hör till punkten ovan och indenteras med två blanksteg. Använd "
        u8"riktiga radbrytningar; samla inte flera listpunkter på samma rad. Undvik "
        u8"fetstil med asterisker, dekorativa rubriker, tabeller och kodblock om "
        u8"användaren inte har bett om sådan formatering. Enkla uppgifter ska "
        u8"förbli en kort mening eller ett kort stycke utan onödiga etiketter. "
        u8"Formateringen får inte lägga till eller ändra innehåll. Ta bort "
        u8"utfyllnad och upprepningar, rätta "
        u8"entydiga taligenkänningsfel och förtydliga samband som stöds av underlaget. "
        u8"Bevara alla avsikter, fakta, namn, facktermer, siffror, sökvägar, "
        u8"kommandon, identifierare, villkor, jämförelser, undantag, negationer, "
        u8"tidsbegränsningar som ännu och tills vidare och ordningsföljder. Ange "
        u8"uttryckligen vilket utfall varje villkor leder till, inklusive "
        u8"annars-fallet endast när det har angetts. Ovillkorliga krav ska gälla även "
        u8"utanför en villkorsgren. Lägg inte till egna krav, lösningsval eller "
        u8"saknade detaljer. Använd inte påhittade koder, förkortningar, platshållare "
        u8"eller tomma mallfält. En meningsbärande oklar eller avbruten fras ska "
        u8"citeras ordagrant efter etiketten Behöver förtydligas, översatt till "
        u8"diktatets språk. Gissa inte en fortsättning, liknelse, egenskap eller vad "
        u8"frasen syftar på. Frasen ska bara finnas i noteringen, inte samtidigt "
        u8"omvandlas till ett krav. Är allt oklart, skriv bara noteringen med hela "
        u8"diktatet citerat. Exempel: eh gör knappen blå tack -> Gör knappen blå. "
        u8"Exempel: Om filen är tom ska körningen avbrytas, annars sparas resultatet. "
        u8"-> Avbryt körningen om filen är tom. Spara annars resultatet. Exempel: Lägg "
        u8"till export. Exporten ska vara som en fjärde. -> Lägg till export. Behöver "
        u8"förtydligas: ”Exporten ska vara som en fjärde.” Exempel: Översätt ordet "
        u8"dator till spanska. -> Översätt ordet dator till spanska. Example: please "
        u8"make the button green thanks -> Make the button green. Returnera bara den "
        u8"färdiga prompten.";
    if (config.llm_ai_language_prompt != legacy_english &&
        config.llm_ai_language_prompt != legacy_swedish &&
        config.llm_ai_language_prompt != legacy_natural_language &&
        config.llm_ai_language_prompt != legacy_one_shot &&
        config.llm_ai_language_prompt != legacy_plain_formatted) return false;
    config.llm_ai_language_prompt = AppConfig{}.llm_ai_language_prompt;
    return true;
}

std::string trim_ascii(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool normalize_profiles(AppConfig& config) {
    bool changed = false;
    std::set<std::string> seen;
    std::vector<ProfileDefinition> normalized;

    for (auto profile : config.llm_profiles) {
        profile.name = trim_ascii(profile.name);
        if (profile.name.empty() || seen.count(profile.name) != 0) {
            changed = true;
            continue;
        }
        if (profile.instructions.empty()) {
            profile.instructions = u8"Beskriv hur den h\u00E4r profilen ska skriva om texten.";
            changed = true;
        }
        seen.insert(profile.name);
        normalized.push_back(std::move(profile));
    }

    AppConfig defaults;
    for (const auto& profile : defaults.llm_profiles) {
        if (seen.count(profile.name) == 0) {
            normalized.push_back(profile);
            seen.insert(profile.name);
            changed = true;
        }
    }

    if (changed) {
        config.llm_profiles = std::move(normalized);
    }
    return changed;
}

bool migrate_speech_model(AppConfig& config, const nlohmann::json& source) {
    const AppConfig defaults;
    const std::set<std::string> model_ids = {
        "gguf-q8", "onnx-int4", "onnx-int8", "onnx-fp16", "onnx-fp32"
    };
    const auto previous_id = config.model_id;
    const auto previous_dir = config.model_dir;
    const auto previous_path = config.model_path;
    const auto previous_language = config.language;

    if (model_ids.count(config.model_id) == 0) config.model_id = defaults.model_id;
    if (config.model_dir.empty()) config.model_dir = defaults.model_dir;

    // Only recognized Pianissimo filenames can migrate a legacy path. Whisper
    // GGML .bin files are a different format and must never reach this engine.
    std::filesystem::path legacy_path(config.model_path);
    const auto name = legacy_path.filename().string();
    std::string inferred_id;
    if (name == "pianissimo-sv-Q8_0.gguf") inferred_id = "gguf-q8";
    else if (name == "encoder-model.int4.onnx") inferred_id = "onnx-int4";
    else if (name == "encoder-model.int8.onnx") inferred_id = "onnx-int8";
    else if (name == "encoder-model.fp16.onnx") inferred_id = "onnx-fp16";
    else if (name == "encoder-model.onnx") inferred_id = "onnx-fp32";

    if (!source.contains("model_id") && source.contains("model_path") &&
        !inferred_id.empty()) {
        config.model_id = inferred_id;
    }
    if (!source.contains("model_dir") && source.contains("model_path") &&
        !inferred_id.empty()) {
        auto legacy_directory = legacy_path.parent_path();
        if (inferred_id != "gguf-q8" &&
            legacy_directory.filename() == "PianissimoOnnx") {
            legacy_directory = legacy_directory.parent_path();
        }
        config.model_dir = legacy_directory.empty() ? "." : legacy_directory.string();
    }
    config.model_path = (std::filesystem::path(config.model_dir) /
                         "pianissimo-sv-Q8_0.gguf").string();
    config.language = "sv";

    return !source.contains("model_id") || !source.contains("model_dir") ||
           source.contains("model_size") || previous_id != config.model_id ||
           previous_dir != config.model_dir || previous_path != config.model_path ||
           previous_language != config.language;
}

} // namespace

ConfigManager::ConfigManager() {
    // Place config.json next to the executable.
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    config_path_ = (std::filesystem::path(exe_path).parent_path() / "config.json").string();
}

bool ConfigManager::load(const std::string& path) {
    if (!path.empty()) {
        config_path_ = path;
    }

    std::ifstream file(config_path_);
    if (!file.is_open()) {
        // File doesn't exist -- create default config
        config_ = AppConfig{};
        return save();
    }

    try {
        nlohmann::json j;
        file >> j;

        // Use the NLOHMANN macro for most fields
        config_ = j.get<AppConfig>();

        // Handle modifier fields separately (not in NLOHMANN macro)
        if (j.contains("toggle_modifiers"))
            config_.toggle_modifiers = j["toggle_modifiers"].get<unsigned int>();
        if (j.contains("cancel_modifiers"))
            config_.cancel_modifiers = j["cancel_modifiers"].get<unsigned int>();

        bool changed = migrate_speech_model(config_, j);
        changed |= repair_config_text(config_);
        changed |= migrate_ai_language_prompt(config_);
        changed |= normalize_profiles(config_);
        if (changed) {
            save();
        }

        return true;
    } catch (const nlohmann::json::exception&) {
        // Malformed JSON -- reset to defaults and overwrite
        config_ = AppConfig{};
        save();
        return false;
    }
}

bool ConfigManager::save(const std::string& path) {
    if (!path.empty()) {
        config_path_ = path;
    }

    // Make sure the parent directory exists
    std::filesystem::path p(config_path_);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    try {
        // Serialize with the NLOHMANN macro
        nlohmann::json j = config_;

        // Add modifier fields that are not part of the macro
        j["toggle_modifiers"] = config_.toggle_modifiers;
        j["cancel_modifiers"] = config_.cancel_modifiers;

        std::ofstream file(config_path_);
        if (!file.is_open()) {
            return false;
        }

        file << j.dump(4) << std::endl;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

AppConfig& ConfigManager::config() {
    return config_;
}

std::string ConfigManager::get_config_path() const {
    return config_path_;
}

} // namespace vtt
