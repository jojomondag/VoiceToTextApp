#include "config_manager.h"
#include "live_text_format.h"
// Include the production implementation to exercise its response validation
// without requiring a network server for the normal regression suite.
#include "../src/llm_cleaner.cpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void direct_typing_format() {
    const std::string structured =
        u8"\r\nMål: Bygg ett spel.\r\n\r\nKrav:\r\n"
        u8"    1. Behåll 14 karaktärer.\n\t2. Ändra inte C:\\Users\\Josef\\spel.json.\n"
        u8"Behöver förtydligas: \"som en tredje\"\r\n";
    const std::string expected =
        u8"Mål: Bygg ett spel. Krav: 1. Behåll 14 karaktärer. "
        u8"2. Ändra inte C:\\Users\\Josef\\spel.json. "
        u8"Behöver förtydligas: \"som en tredje\"";
    require(vtt::prompt_for_direct_typing(structured) == expected,
            "structured prompts must flatten without losing requirements or literal values");
    const std::string single_line = u8"Behåll x[2], API_KEY, 2500 och C:\\Users\\Josef\\fil.txt exakt.";
    require(vtt::prompt_for_direct_typing(single_line) == single_line,
            "single-line content must remain unchanged");
    require(vtt::prompt_for_direct_typing("\r\n\r\n").empty(),
            "empty lines must not become text or submission keys");
}

void reasoning_request_and_response() {
    vtt::LlmCleanerConfig settings;
    settings.max_tokens = 4096;
    const auto ordinary = nlohmann::json::parse(
        vtt::build_request_body(settings, "Rewrite faithfully.", "Short request."));
    const auto ai_prompt = nlohmann::json::parse(
        vtt::build_request_body(settings, "Rewrite faithfully.", "Short request.", true));
    require(ordinary.at("max_tokens") == 512 &&
            ordinary.at("chat_template_kwargs").at("enable_thinking") == false,
            "ordinary cleanup must retain its short non-reasoning budget");
    require(ai_prompt.at("max_tokens") == 4096 &&
            ai_prompt.at("chat_template_kwargs").at("enable_thinking") == true,
            "AI prompts need reasoning with enough budget for final content");
    settings.max_tokens = 2048;
    const auto configured = nlohmann::json::parse(
        vtt::build_request_body(settings, "Rewrite faithfully.", "Short request.", true));
    require(configured.at("max_tokens") == 2048,
            "reasoning must honor the configured token ceiling");

    std::string output;
    require(vtt::parse_cleaned_text(
        R"({"choices":[{"finish_reason":"stop","message":{"reasoning_content":"Private model reasoning.","content":"Make the button blue."}}]})",
        output) && output == "Make the button blue.",
        "only final content may reach the user's text field");
    require(!vtt::parse_cleaned_text(
        R"({"choices":[{"finish_reason":"stop","message":{"reasoning_content":"Reasoning only.","content":""}}]})",
        output), "reasoning without final content must not replace raw dictation");
}

void notation_validation() {
    const std::string source = u8"Bygg Teachers Little Helper för alla elever i klassrummet.";
    for (const auto& output : {"Bygg TLH:TA_ALL_STU.", "Bygg [PROGRAM].",
                               "[INGEN DATA]", "Ange [RESULTAT] och [KUNNA].",
                               u8"Annars: [Ingen åtgärd specificerad]",
                               u8"Lägg till [ett resultat här]."}) {
        require(vtt::rewrite_has_invented_notation(source, output),
                "invented notation must be rejected");
    }
    require(!vtt::rewrite_has_invented_notation(source,
        u8"Implementera Teachers Little Helper för samtliga elever i klassrummet."),
        "ordinary natural language must be accepted");
    require(!vtt::rewrite_has_invented_notation(
        "Behall TLH:TA_ALL_STU och [RESULTAT] i API:t.",
        "Behall TLH:TA_ALL_STU och [RESULTAT] i API:t."),
        "notation explicitly supplied by the user must remain valid");
    require(!vtt::rewrite_has_invented_notation("Anvand API_KEY i app.cpp.",
        "Anvand API_KEY i app.cpp."), "supplied identifiers must be valid");
    require(!vtt::rewrite_has_invented_notation("Anvand x[2] och arr[i].",
        "Anvand x[2] och arr[i]."), "ordinary indexing must remain valid");
    require(!vtt::rewrite_has_invented_notation("Bevara [resultat].",
        "Bevara [RESULTAT]."), "label casing must not cause rejection");
    require(!vtt::rewrite_has_invented_notation(u8"Skriv [Ingen åtgärd specificerad] exakt.",
        u8"Behåll texten [Ingen åtgärd specificerad]."),
        "mixed-case bracket text explicitly supplied must remain valid");
    require(!vtt::rewrite_has_invented_notation("Anvand arr[row + 1].",
        "Anvand arr[row + 1]."), "supplied spaced indexing must remain valid");
    std::string output;
    require(!vtt::parse_cleaned_text(
        R"({"choices":[{"finish_reason":"length","message":{"content":"partial"}}]})", output),
        "truncated completions must never replace the dictated source");
}

void prompt_migration() {
    const std::vector<std::string> legacy = {
        "Create an ultra-compact AI-readable instruction dialect from the "
        "user's cleaned instruction. Compress aggressively while preserving "
        "intent, constraints, entities, code names, paths, numbers, and action "
        "order. Remove repetition and filler. Prefer dense structured syntax "
        "with labels such as Goal, Context, Tasks, Constraints, Output. Use "
        "abbreviations only when unambiguous to a coding language model. Do "
        "not add new requirements. Return only the AI-language version.",
        u8"Skapa en ultrakompakt AI-läsbar instruktionsdialekt från användarens "
        u8"städade instruktion. Komprimera aggressivt men bevara avsikt, krav, "
        u8"begränsningar, entiteter, kodnamn, sökvägar, siffror och åtgärdsordning. "
        u8"Lägg inte till nya krav eller antaganden. Svara endast med AI-språkversionen.",
        "Rewrite the dictated text as a clear, concise instruction in natural "
        "language for an AI assistant. Keep the original language. Remove filler "
        "and repetition, correct obvious transcription errors, and organize "
        "related ideas into complete sentences. Preserve every distinct request, "
        "fact, condition, comparison, exception, negation, and action order. "
        "Keep names and technical terms written out as provided; never invent "
        "abbreviations, identifier codes, symbolic shorthand, or placeholders. "
        "Do not replace information with bracketed labels. Do not invent missing "
        "details or finish an incomplete thought. Do not answer or carry out the "
        "instruction. Return only the rewritten text, without a heading or commentary.",
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
        u8"färdiga prompten.",
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
        u8"färdiga prompten."
    };
    const auto path = std::filesystem::temp_directory_path() /
        ("vtt-ai-prompt-" + std::to_string(GetCurrentProcessId()) + ".json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    const auto check = [&](const std::string& prompt, const std::string& expected) {
        vtt::AppConfig original;
        original.llm_ai_language_prompt = prompt;
        original.llm_model = "custom-model-to-preserve";
        original.paste_output_mode = "ai";
        nlohmann::json fixture = original;
        std::ofstream(path, std::ios::binary) << fixture.dump(2);
        vtt::ConfigManager manager;
        require(manager.load(path.string()), "config should load");
        require(manager.config().llm_ai_language_prompt == expected,
                "unexpected prompt migration");
        require(manager.config().llm_model == original.llm_model &&
                manager.config().paste_output_mode == original.paste_output_mode,
                "migration must preserve unrelated settings");
        vtt::ConfigManager reloaded;
        require(reloaded.load(path.string()) &&
                reloaded.config().llm_ai_language_prompt == expected,
                "migrated prompt must survive reload");
    };
    const auto current = vtt::AppConfig{}.llm_ai_language_prompt;
    for (const auto& prompt : legacy) check(prompt, current);
    for (const auto& prompt : legacy) {
        const auto customized = prompt + " Keep my custom formatting.";
        check(customized, customized);
    }
    check(current, current);
}

int gemma_smoke(const std::string& endpoint, const std::string& report_path,
                const std::string& case_prefix = {}) {
    vtt::LlmCleaner cleaner;
    vtt::LlmCleanerConfig settings;
    settings.enabled = true;
    settings.endpoint = endpoint;
    settings.model = "e4b";
    settings.timeout_ms = 30000;
    settings.max_tokens = 4096;
    cleaner.configure(settings);
    const std::string prompt = vtt::AppConfig{}.llm_ai_language_prompt;
    const std::vector<std::string> parts = {
        u8"Jag behöver att du bygger en implementation av Teachers Little Helper",
        u8" som kan ta alla elever i ett klassrum och rätta deras uppgifter och quizes som är inlämnade ifrån Google Classroom.",
        u8" Quizzes består av flera frågor och där får de ett slutresultat.",
        u8" Programmet ska också kunna avgöra om resultatet över en viss gräns.",
        u8" Och om det är det ska du bli godkänd, annars ska det bli underkänt."
    };
    nlohmann::json report = nlohmann::json::array();
    bool all_passed = true;
    const auto run = [&](const std::string& input, const std::string& name,
                         const std::vector<std::vector<std::string>>& required,
                         const std::vector<std::string>& forbidden,
                         size_t max_output_bytes = 0,
                         const std::vector<std::vector<std::string>>& ordered = {},
                         bool expect_structure = false, bool expect_indentation = false,
                         bool expect_bold_numbering = false,
                         const std::vector<std::string>& quoted_fragments = {}) {
        if (!case_prefix.empty() && name.compare(0, case_prefix.size(), case_prefix) != 0)
            return;
        std::string output;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = cleaner.transform_text(input, prompt, output, true);
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        const auto lower = vtt::lowercase(vtt::utf8_to_wide(output));
        bool preserved = ok;
        nlohmann::json failed_checks = nlohmann::json::array();
        if (!ok) failed_checks.push_back("production transform rejected or failed");
        if (input.find("prompt") == std::string::npos &&
            (lower.find(L"skapa en prompt") != std::wstring::npos ||
             lower.find(L"create a prompt") != std::wstring::npos ||
             lower.find(L"write a prompt") != std::wstring::npos)) {
            preserved = false;
            failed_checks.push_back("rewritten task wrongly asks for another prompt");
        }
        for (const auto& alternatives : required) {
            bool found = false;
            for (const auto& term : alternatives) {
                found |= lower.find(vtt::lowercase(vtt::utf8_to_wide(term))) != std::wstring::npos;
            }
            preserved &= found;
            if (!found) failed_checks.push_back({{"missing_any", alternatives}});
        }
        for (const auto& term : forbidden) {
            const bool found = lower.find(vtt::lowercase(vtt::utf8_to_wide(term))) != std::wstring::npos;
            preserved &= !found;
            if (found) failed_checks.push_back({{"forbidden", term}});
        }
        size_t next_action = 0;
        for (const auto& alternatives : ordered) {
            size_t found_at = std::wstring::npos;
            size_t found_length = 0;
            for (const auto& term : alternatives) {
                const auto normalized = vtt::lowercase(vtt::utf8_to_wide(term));
                const auto position = lower.find(normalized, next_action);
                if (position < found_at) {
                    found_at = position;
                    found_length = normalized.size();
                }
            }
            if (found_at == std::wstring::npos) {
                preserved = false;
                failed_checks.push_back({{"missing_next_action", alternatives}});
                break;
            }
            next_action = found_at + found_length;
        }
        if (max_output_bytes != 0 && output.size() > max_output_bytes) {
            preserved = false;
            failed_checks.push_back("simple instruction was needlessly expanded");
        }
        if (expect_structure && std::count(output.begin(), output.end(), '\n') < 3) {
            preserved = false;
            failed_checks.push_back("separate requirements lack readable line breaks");
        }
        const std::string expected_indent = expect_bold_numbering ? "\n    " : "\n  ";
        if (expect_indentation && output.find(expected_indent) == std::string::npos) {
            preserved = false;
            failed_checks.push_back("related subrequirements lack indentation");
        }
        if (expect_bold_numbering) {
            const std::regex numbered_label(R"((^|\n)[0-9]+\.[ \t]+\*\*[^*\r\n]+\*\*)");
            const auto count = std::distance(std::sregex_iterator(output.begin(), output.end(), numbered_label),
                std::sregex_iterator());
            if (count < 2) {
                preserved = false;
                failed_checks.push_back("main requirements lack numbered lines with bold labels");
            }
        }
        std::string straight_quotes = output;
        for (const std::string quote : {u8"“", u8"”"}) {
            size_t position = 0;
            while ((position = straight_quotes.find(quote, position)) != std::string::npos) {
                straight_quotes.replace(position, quote.size(), "\"");
                ++position;
            }
        }
        for (const auto& fragment : quoted_fragments) {
            bool quoted = false;
            size_t opening = 0;
            while ((opening = straight_quotes.find('"', opening)) != std::string::npos) {
                const auto closing = straight_quotes.find('"', opening + 1);
                if (closing == std::string::npos) break;
                const auto position = straight_quotes.find(fragment, opening + 1);
                quoted |= position != std::string::npos && position + fragment.size() <= closing;
                opening = closing + 1;
            }
            if (!quoted) {
                preserved = false;
                failed_checks.push_back({{"ambiguity_not_quoted_verbatim", fragment}});
            }
        }
        all_passed &= preserved;
        report.push_back({{"case", name}, {"input", input}, {"output", output},
            {"ok", ok}, {"requirements_preserved", preserved}, {"milliseconds", duration},
            {"failed_checks", failed_checks}});
    };
    std::string input;
    std::vector<std::vector<std::string>> required = {
        {"Teachers Little Helper"}, {"bygg", "implementera", "utveckla", "skapa"}};
    for (size_t stage = 0; stage < parts.size(); ++stage) {
        input += parts[stage];
        if (stage == 1) {
            required.push_back({"alla elever", "samtliga elever"});
            required.push_back({"klass"});
            required.push_back({"uppgifter"});
            required.push_back({"quiz"});
            required.push_back({"Google Classroom"});
        }
        if (stage == 2) {
            required.push_back({u8"flera frågor"});
            required.push_back({"slutresultat"});
        }
        if (stage == 3) required.push_back({u8"över en", u8"överskrider en", u8"överstiger en"});
        if (stage == 4) {
            required.push_back({u8"godkän"});
            required.push_back({u8"underkän"});
            required.push_back({"annars", "i annat fall", u8"inte är över gränsen", u8"inte överstiger gränsen"});
        }
        run(input, "growing_source_" + std::to_string(stage + 1), required,
            {"TLH:", "[", ">=", u8"minst", "50", "60"});
    }
    run(u8"Sätt gränsen senare. Bestäm inte någon poänggräns nu.", "missing_threshold",
        {{u8"gräns"}, {"inte", "ej", "ingen"}}, {"50", "60", "70", "["});
    run(u8"Behåll identifieraren TLH:TA_ALL_STU och variabeln API_KEY exakt. Ändra inte C:\\Users\\Josef\\projekt.txt.",
        "supplied_identifiers", {{"TLH:TA_ALL_STU"}, {"API_KEY"},
         {"C:\\Users\\Josef\\projekt.txt"}, {"inte", "ej", u8"oförändr"}}, {});
    run(u8"boka ett möte med Anna på fredag klockan 14 skicka inte inbjudan till Erik och budgeten får inte överstiga 2500 kronor",
        "negations_and_details", {{"Anna"}, {"fredag"}, {"14"}, {"Erik"}, {"2500"}, {"inte", "ej"}}, {});
    run(u8"Bygg ett spel som ska ha fjorton karaktärer, och dessa karaktärer ska ha olika förmågor, texturer och tredimensionella modeller på texturerna. De ska också kunna spawnas på olika sätt och behöver då vara som en tredje.",
        "confirmed_game_term_with_ambiguous_ending",
        {{"spel"}, {"fjorton", "14"}, {u8"karaktär"}, {u8"förmågor"}, {"textur"},
         {"tredimensionella modeller", "3D-modeller"}, {"spawnas"}, {u8"olika sätt"},
         {"som en tredje"}, {u8"förtydlig", "oklar", u8"ofullständig"}},
        {"tredje part", "tredjeperson", "tredje person", u8"spåras", "spanas", "Unity", "Unreal", "["});
    run(u8"De behöver då vara som en tredje.", "ambiguous_fragment",
        {{"som en tredje"}, {u8"förtydlig", "oklar", u8"ofullständig"}},
        {"tredje part", "tredjeperson", "tredje person", "["});
    run(u8"Och sen vill jag att", "unfinished_fragment",
        {{"vill jag att"}}, {"funktion", "knapp", "["}, 250);
    run(u8"eh gör knappen blå tack", "simple_task_stays_compact",
        {{"knappen"}, {u8"blå"}}, {"CSS", "hex", "#", "["}, 100);
    run(u8"Bygg en exporterare. Indata är elever.csv. Svara med en enda Python-fil utan externa bibliotek. Ändra inte indatafilen. Hoppa över tomma rader men behåll elever utan e-post. Resultatet ska vara JSON med exakt fälten id och namn.",
        "explicit_output_constraints_and_exception",
        {{"export"}, {"elever.csv"}, {"Python"}, {"utan externa bibliotek", "inga externa bibliotek", u8"inte använda några externa bibliotek"},
         {u8"ändra inte", u8"ändras inte", u8"oförändr", u8"får inte ändras", u8"inte ändra"},
         {"tomma rader"}, {u8"utan e-post", u8"saknar e-post"}, {"JSON"}, {"id"}, {"namn"}},
        {"pandas", "["});
    run(u8"Läs först config.json. Kontrollera sedan att porten är 8088. Starta servern först om kontrollen lyckas, och radera aldrig filen.",
        "action_order_and_condition",
        {{"config.json"}, {"8088"}, {"server"}, {"om kontrollen lyckas", u8"endast om", u8"bara om"}, {"aldrig", "inte"}},
        {"["});
    run(u8"Översätt meningen hej världen till engelska och ge endast översättningen.",
        "translation_request_is_not_executed",
        {{u8"översätt"}, {u8"hej världen"}, {"engelska"}},
        {"Hello world", "Hello, world", "["});
    run(u8"Bygg ett spel med tolv karaktärer. Nej jag menar fjorton karaktärer. Använd inga externa bibliotek.",
        "explicit_self_correction",
        {{"spel"}, {"fjorton", "14"}, {"inga externa bibliotek", "utan externa bibliotek"}},
        {"tolv", "12", "["});
    run(u8"Bygg en rapport från filen jag nämnde tidigare, använd samma gräns som förut, men skicka den inte ännu.",
        "unresolved_references_do_not_become_details",
        {{"rapport"}, {"fil"}, {u8"gräns"}, {"tidigare", u8"förut"}, {"inte"},
         {u8"ännu", u8"inte än", "senare", u8"för tillfället"}},
        {"CSV", "PDF", "50", "60", "70", "["});
    run("Build a CSV exporter in one Python file. Do not modify input.csv. Keep empty email addresses, and return JSON containing only id and name.",
        "english_input_remains_english",
        {{"build", "implement", "create"}, {"CSV"}, {"Python"}, {"input.csv"},
         {"do not", "must not", "unchanged"}, {"empty email"}, {"JSON"}, {"id"}, {"name"}},
        {"[", "pandas"});
    const std::vector<std::string> build_trigger = {
        u8"när jag bygger appen", u8"när appen byggs", u8"när applikationen byggs",
        u8"vid bygge av appen", u8"vid byggandet av appen", u8"vid varje bygge",
        u8"när ett bygge", u8"vid byggstart", u8"när byggprocessen", u8"när jag bygger applikationen"};
    run(u8"Jag vill att när jag bygger appen så ska den känna av att det finns en ny uppdatering som behövs göras och att den automatiskt Kalla på detta genom att stänga applikationen och sedan uppdatera den applikationerna och sen bygger igen.",
        "update_build_trigger_and_action_order",
        {build_trigger, {"ny uppdatering", "nya uppdateringar", "uppdatering finns", "uppdateringar finns"},
         {"automatiskt"}},
        {"ladda ner", "laddas ner", u8"återstart", u8"starta om", "startas om", "lyckas", "lyckat", "["}, 0,
        {{u8"kontroll", u8"känna av", u8"upptäck", u8"identifier", u8"undersök", "ny uppdatering", "nya uppdateringar"},
         {u8"stäng"}, {"uppdater"}, {"bygg"}});
    run(u8"Jag vill att när jag bygger appen så ska den känna av att det finns en ny uppdatering som behövs göras och att den automatiskt",
        "update_incomplete_prefix_keeps_build_trigger",
        {build_trigger, {"ny uppdatering", "nya uppdateringar", "uppdatering finns", "uppdateringar finns"},
         {"automatiskt"}},
        {u8"stäng", "ladda ner", u8"återstart", u8"starta om", "startas om", "["});
    run(u8"Implementera språkigenkänning för röstinmatningen. Användaren ska kunna välja mellan två lägen. Automatiskt läge skriver på det språk användaren talar. Fast målspråk skriver alltid på användarens valda språk och översätter vid behov. I båda lägena ska namn och siffror bevaras. Visa vilket läge som är aktivt.",
        "bold_language_modes",
        {{u8"språkigenkänning"}, {u8"röstinmatning"}, {"automatiskt"}, {u8"fast målspråk"},
         {u8"valda språk", u8"valt språk"}, {u8"översätt"}, {"namn"}, {"siffror"}, {"aktivt"}},
        {"Whisper", "API", "engelska", "svenska", "["}, 0, {}, true, true, true);
    run(u8"Först läs config.json. Sedan kontrollera att porten är 8088. Om kontrollen lyckas ska servern startas, annars ska ett felmeddelande visas. Ändra inte konfigurationsfilen. Radera aldrig loggarna.",
        "formatting_ordered_conditional_steps",
        {{"config.json"}, {"8088"}, {u8"lyckas"}, {"annars"}, {"felmeddelande"},
         {u8"ändra inte", u8"får inte ändras", u8"oförändr", u8"inte ändra"}, {"aldrig", "inte"}, {"logg"}},
        {"["}, 0, {{u8"läs"}, {"kontroll"}, {"start"}}, true);
    run(u8"Vi testar nu den nya AI-modellen. Om jag vill bygga vidare på den här så vill jag kunna lägga till flera språk. De här språken ska man sen kunna välja emellan och när man närma sen Prata på det språket ska detta språket i sin tur översättas till svenska med pianagnissimo. Så vi behöver på något sätt detektionsmodeller som kan avgöra. vad vi ska ha för språk. Först måste vi bygga två språk, sen måste vi lägga till fyra och därefter får vi ta en fjärde språk. och allt det här behöver fungera liksom i vart annat. Mm.",
        "bold_actual_multilingual_dictation_with_ambiguous_counts",
        {{"AI-modell"}, {u8"flera språk", u8"flerspråk"}, {u8"välja", "val"},
         {u8"översätt"}, {"svenska"}, {"pianagnissimo"}, {"detek"}, {u8"förtydlig", "oklar"}},
        {"ett ytterligare", "stilen pianagnissimo", "ton av pianagnissimo", "["}, 0, {}, true, false, true,
        {u8"Först måste vi bygga två språk, sen måste vi lägga till fyra och därefter får vi ta en fjärde språk."});
    run(u8"eh gör knappen blå tack", "bold_simple_task_stays_compact",
        {{"knappen"}, {u8"blå"}}, {"CSS", "hex", "#", "[", "**"}, 100);
    if (report.empty()) throw std::runtime_error("no Gemma smoke cases matched the requested prefix");
    nlohmann::json result = {{"system_prompt", prompt}, {"enable_thinking", true},
        {"max_tokens", settings.max_tokens}, {"case_prefix", case_prefix},
        {"checks", report}, {"pass", all_passed}};
    std::ofstream(report_path, std::ios::binary) << result.dump(2);
    std::cout << result.dump(2) << '\n';
    return all_passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--gemma")
            return gemma_smoke(argv[2], argv[3], argc == 5 ? argv[4] : "");
        notation_validation();
        prompt_migration();
        direct_typing_format();
        reasoning_request_and_response();
        std::cout << "PASS: rewrite validation, prompt migration, direct-typing format and reasoning budgets\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
