#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <windows.h>

namespace vtt {

struct ProfileDefinition {
    std::string name;
    std::string instructions;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ProfileDefinition,
        name, instructions)
};

struct AppConfig {
    // Pianissimo Swedish speech models. All selected variant files share model_dir.
    std::string model_id = "onnx-int8";
    std::string model_dir = "models/Pianissimo";
    // Compatibility alias for older configurations; ONNX uses model_dir instead.
    std::string model_path = "models/Pianissimo/pianissimo-sv-Q8_0.gguf";
    std::string language = "sv";         // Pianissimo supports Swedish
    bool use_gpu = false;
    std::string compute_mode = "auto";   // "auto", "gpu", "cpu"
    int n_threads = 0;                    // 0 = auto
    bool whisper_fast_mode = true;
    bool warm_up_model_on_load = true;

    // Audio settings
    std::string audio_device;            // empty = default
    float vad_threshold = 0.01f;
    bool trim_silence_before_transcription = true;
    int trim_silence_padding_ms = 250;

    // Hotkey settings (stored as virtual key codes + modifiers)
    unsigned int toggle_modifiers = MOD_CONTROL | MOD_SHIFT;
    unsigned int toggle_key = VK_SPACE;
    unsigned int cancel_modifiers = 0;
    unsigned int cancel_key = VK_ESCAPE;

    // Text injection
    std::string injection_method = "auto"; // "auto", "clipboard", "keystrokes"
    int keystroke_delay_ms = 5;
    int pre_injection_delay_ms = 25;
    std::string paste_output_mode = "clean"; // "ai", "recorded", "clean"
    // Local bounded diagnostics include only dictated/inserted spans and cursor metadata.
    bool dictation_debug_logging = true;

    // Local LLM cleanup
    bool llm_cleanup_enabled = false;
    std::string llm_provider = "lmstudio"; // "lmstudio", "internal"
    std::string llm_endpoint = "http://127.0.0.1:1234/v1/chat/completions";
    std::string llm_model = "local-model";
    std::string llm_internal_model = "auto";
    std::string llm_internal_model_path = "models/llm/gemma-4-E2B-it-Q8_0.gguf";
    std::string llm_internal_compute_mode = "auto";
    std::string llm_internal_server_path = "llama-server.exe";
    int llm_internal_port = 8088;
    bool llm_internal_auto_start = true;
    std::string llm_pre_prompt =
        "You clean up dictated instructions before they are sent to an AI "
        "coding model. Fix transcription mistakes, wrong words that clearly "
        "do not fit the context, punctuation, casing, grammar, and structure. "
        "Make the text clear, explicit, and easy for a coding model to act on. "
        "Use coding-context corrections when obvious, for example LM Studio, "
        "app.cpp, C++, CMake, PowerShell, JSON, API, endpoint, and config. "
        "Preserve the user's intent, language, tone, names, paths, commands, "
        "code identifiers, numbers, and technical terms. Do not add new "
        "requirements or explanations. Return only the cleaned instruction.";
    int llm_timeout_ms = 30000;
    int llm_max_tokens = 4096;          // Dynamic per-request ceiling
    bool llm_corrections_enabled = true;
    std::string llm_corrections_path = "transcription_corrections.json";
    int llm_corrections_max_entries = 200;
    int llm_corrections_prompt_limit = 20;
    int llm_corrections_min_count = 1;
    bool llm_ai_language_enabled = true;
    std::string llm_ai_language_prompt =
        u8"Du redigerar dikterat tal till en färdig prompt för en annan AI. Målet är "
        u8"att AI:n ska förstå användarens avsikt och kunna utföra uppgiften i ett "
        u8"försök, så långt underlaget räcker. Skriv om instruktionen; besvara eller "
        u8"utför den inte. Skriv på diktatets språk, inte språket i dessa regler: "
        u8"svenska in ger svenska ut och engelska in ger engelska ut. Byt bara språk "
        u8"om användaren uttryckligen ber om själva prompten på ett annat språk. En "
        u8"begäran att AI:n ska översätta någon annan text ska förbli en instruktion "
        u8"på diktatets språk. Gör målet tydligt och samla relaterade krav. Använd "
        u8"naturliga meningar och en layout som är lätt att läsa för både människor "
        u8"och AI. Vid flera krav: skriv en kort inledning med målet och sedan en "
        u8"numrerad lista med huvudkraven (1., 2., 3.). Skriv varje huvudkrav på en "
        u8"egen rad med en kort beskrivande etikett i fetstil, markerad med dubbla "
        u8"asterisker, följd av kolon och själva kravet. Numreringen underlättar "
        u8"läsningen men får inte lägga till en utförandeordning som användaren "
        u8"inte har angett. Sätt en tomrad mellan inledningen och listan samt "
        u8"mellan huvudpunkter och avsnitt. Gruppera bara när det behövs; lägg inte "
        u8"till obligatoriska mallavsnitt. Underpunkter ska bara användas för "
        u8"detaljer som hör till huvudpunkten ovan. Skriv dem som punkter med "
        u8"bindestreck, indenterade med fyra blanksteg. Använd riktiga radbrytningar "
        u8"och bevara indenteringen; samla inte flera listpunkter på samma rad. "
        u8"Undvik tabeller och kodblock om användaren inte har bett om dem. Enkla "
        u8"uppgifter ska "
        u8"förbli en kort mening eller ett kort stycke utan onödiga etiketter. "
        u8"Formateringen får inte lägga till eller ändra innehåll. Ta bort "
        u8"utfyllnad och upprepningar, rätta "
        u8"entydiga taligenkänningsfel och förtydliga samband som stöds av underlaget. "
        u8"Bevara alla avsikter, fakta, namn, facktermer, siffror, sökvägar, "
        u8"kommandon, identifierare, villkor, jämförelser, undantag, negationer, "
        u8"tidsbegränsningar som ännu och tills vidare och ordningsföljder. Bevara "
        u8"vem som talar och vem som ska göra vad; byt inte aktörernas roller. "
        u8"Bevara vilket språk som är indata respektive utdata. Ange "
        u8"uttryckligen vilket utfall varje villkor leder till, inklusive "
        u8"annars-fallet endast när det har angetts. Ovillkorliga krav ska gälla även "
        u8"utanför en villkorsgren. Lägg inte till egna krav, lösningsval eller "
        u8"saknade detaljer. Okända namn och termer får inte ges en gissad "
        u8"betydelse. Ett obekant verktygs- eller produktnamn får inte omtolkas "
        u8"till en ton, stil eller annan funktion; behåll namnet ordagrant och "
        u8"markera oklart sammanhang som en oklarhet. Använd inte påhittade koder, "
        u8"förkortningar, platshållare "
        u8"eller tomma mallfält. En meningsbärande oklar eller avbruten fras ska "
        u8"citeras ordagrant efter etiketten Behöver förtydligas, översatt till "
        u8"diktatets språk. Vid motsägande eller oklara antal ska hela den berörda "
        u8"frasen citeras i noteringen; räkna inte fram ett totalantal och välj "
        u8"inte en av tolkningarna. Gissa inte en fortsättning, liknelse, egenskap "
        u8"eller vad "
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
    bool llm_intent_profiles_enabled = true;
    std::string llm_intent_detection_prompt =
        u8"Identifiera anv\u00E4ndarens avsikt fr\u00E5n texten och v\u00E4lj exakt en profil: "
        "coding, email, discussion, translation eller general. Svara endast "
        "med strikt JSON: "
        "{\"profile\":\"coding|email|discussion|translation|general\"}.";
    std::string llm_profile_transform_prompt =
        u8"Transformera texten enligt vald profil. F\u00F6lj profilens egna "
        u8"instruktioner noggrant, bevara anv\u00E4ndarens avsikt och l\u00E4gg inte "
        u8"till nya krav. Svara endast med den transformerade texten.";
    std::vector<ProfileDefinition> llm_profiles = {
        {
            "coding",
            u8"G\u00F6r texten konkret, \u00E5tg\u00E4rdsbar och tydlig f\u00F6r en kodmodell."
        },
        {
            "email",
            u8"G\u00F6r texten till ett tydligt mailutkast med passande ton. "
            u8"L\u00E4gg inte till placeholders, signatur eller \u00E4mnesrad om "
            u8"anv\u00E4ndaren inte bad om det."
        },
        {
            "discussion",
            u8"Bevara samtalston och g\u00F6r resonemanget klart."
        },
        {
            "translation",
            u8"\u00D6vers\u00E4tt eller f\u00F6rbered \u00F6vers\u00E4ttning enligt anv\u00E4ndarens "
            u8"beg\u00E4ran och bevara betydelsen."
        },
        {
            "general",
            u8"St\u00E4da texten utan s\u00E4rskild formatering."
        },
    };

    // UI
    bool show_overlay = true;
    bool auto_start = false;
    std::string transcript_history_path = "transcript_history.json";
    int transcript_history_max_entries = 100;

    // Serialize most fields automatically; modifiers handled separately
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AppConfig,
        model_id, model_dir, model_path, language, use_gpu, compute_mode, n_threads,
        whisper_fast_mode, warm_up_model_on_load,
        audio_device, vad_threshold,
        trim_silence_before_transcription, trim_silence_padding_ms,
        toggle_key, cancel_key,
        injection_method, keystroke_delay_ms, pre_injection_delay_ms,
        paste_output_mode, dictation_debug_logging,
        llm_cleanup_enabled, llm_provider, llm_endpoint, llm_model,
        llm_internal_model, llm_internal_model_path,
        llm_internal_compute_mode, llm_internal_server_path,
        llm_internal_port, llm_internal_auto_start, llm_pre_prompt,
        llm_timeout_ms, llm_max_tokens, llm_corrections_enabled,
        llm_corrections_path, llm_corrections_max_entries,
        llm_corrections_prompt_limit, llm_corrections_min_count,
        llm_ai_language_enabled, llm_ai_language_prompt,
        llm_intent_profiles_enabled, llm_intent_detection_prompt,
        llm_profile_transform_prompt, llm_profiles,
        show_overlay, auto_start,
        transcript_history_path, transcript_history_max_entries)
};

class ConfigManager {
public:
    ConfigManager();

    bool load(const std::string& path = "");
    bool save(const std::string& path = "");

    AppConfig& config();

    std::string get_config_path() const;

private:
    AppConfig config_;
    std::string config_path_;
};

} // namespace vtt
