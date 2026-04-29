#include "llm_cleaner.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <vector>

namespace vtt {
namespace {

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    bool secure = false;
};

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                   static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::string trim(const std::string& text) {
    auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();

    if (begin >= end) return {};
    return std::string(begin, end);
}

bool parse_url(const std::string& url, ParsedUrl& parsed) {
    std::wstring wide_url = utf8_to_wide(url);
    if (wide_url.empty()) return false;

    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);

    wchar_t scheme[16] = {};
    wchar_t host[256] = {};
    wchar_t path[2048] = {};

    parts.lpszScheme = scheme;
    parts.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));

    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()),
                         0, &parts)) {
        return false;
    }

    parsed.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    parsed.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    parsed.port = parts.nPort;

    if (parsed.host.empty() || parsed.path.empty()) return false;
    return parts.nScheme == INTERNET_SCHEME_HTTP ||
           parts.nScheme == INTERNET_SCHEME_HTTPS;
}

std::string build_request_body(const LlmCleanerConfig& config,
                               const std::string& system_prompt,
                               const std::string& input) {
    nlohmann::json body;
    body["model"] = config.model;
    body["temperature"] = 0.1;
    body["max_tokens"] = config.max_tokens;
    body["stream"] = false;
    body["messages"] = nlohmann::json::array({
        {
            {"role", "system"},
            {"content", system_prompt}
        },
        {
            {"role", "user"},
            {"content", input}
        }
    });
    return body.dump();
}

bool parse_cleaned_text(const std::string& response, std::string& output) {
    try {
        auto json = nlohmann::json::parse(response);
        const auto& choices = json.at("choices");
        if (!choices.is_array() || choices.empty()) return false;

        std::string content = choices.at(0).at("message").at("content")
                                  .get<std::string>();
        content = trim(content);
        if (content.empty()) return false;

        output = content;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool parse_profile_content(const std::string& content,
                           LlmProfileResult& result) {
    std::string json_content = trim(content);
    if (json_content.rfind("```", 0) == 0) {
        size_t first_newline = json_content.find('\n');
        size_t last_fence = json_content.rfind("```");
        if (first_newline != std::string::npos &&
            last_fence != std::string::npos &&
            last_fence > first_newline) {
            json_content = trim(json_content.substr(first_newline + 1,
                                                    last_fence - first_newline - 1));
        }
    }

    size_t object_start = json_content.find('{');
    size_t object_end = json_content.rfind('}');
    if (object_start != std::string::npos &&
        object_end != std::string::npos &&
        object_end > object_start) {
        json_content = json_content.substr(object_start,
                                           object_end - object_start + 1);
    }

    try {
        auto json = nlohmann::json::parse(json_content);
        std::string text = trim(json.value("text", ""));
        std::string profile = trim(json.value("profile", "general"));
        if (text.empty()) return false;

        if (profile != "coding" && profile != "email" &&
            profile != "discussion" && profile != "translation" &&
            profile != "general") {
            profile = "general";
        }

        result.text = text;
        result.profile = profile;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool is_valid_profile(const std::string& profile) {
    return profile == "coding" || profile == "email" ||
           profile == "discussion" || profile == "translation" ||
           profile == "general";
}

bool parse_profile_label(const std::string& content, std::string& profile) {
    std::string cleaned = trim(content);
    if (cleaned.rfind("```", 0) == 0) {
        size_t first_newline = cleaned.find('\n');
        size_t last_fence = cleaned.rfind("```");
        if (first_newline != std::string::npos &&
            last_fence != std::string::npos &&
            last_fence > first_newline) {
            cleaned = trim(cleaned.substr(first_newline + 1,
                                          last_fence - first_newline - 1));
        }
    }

    size_t object_start = cleaned.find('{');
    size_t object_end = cleaned.rfind('}');
    if (object_start != std::string::npos &&
        object_end != std::string::npos &&
        object_end > object_start) {
        try {
            auto json = nlohmann::json::parse(
                cleaned.substr(object_start, object_end - object_start + 1));
            std::string parsed = trim(json.value("profile", "general"));
            if (!is_valid_profile(parsed)) parsed = "general";
            profile = parsed;
            return true;
        } catch (const nlohmann::json::exception&) {
        }
    }

    std::string lowered;
    lowered.reserve(cleaned.size());
    for (unsigned char c : cleaned) {
        if (std::isalpha(c)) lowered.push_back(static_cast<char>(std::tolower(c)));
    }

    if (!is_valid_profile(lowered)) return false;
    profile = lowered;
    return true;
}

} // namespace

void LlmCleaner::configure(const LlmCleanerConfig& config) {
    config_ = config;
}

bool LlmCleaner::clean_text(const std::string& input,
                            const std::string& learned_corrections,
                            std::string& output) const {
    LlmProfileResult profile_result;
    if (clean_text_with_profile(input, learned_corrections, profile_result)) {
        output = profile_result.text;
        return true;
    }
    return false;
}

bool LlmCleaner::clean_text_with_profile(const std::string& input,
                                         const std::string& learned_corrections,
                                         LlmProfileResult& result) const {
    result.profile = "general";
    if (config_.intent_profiles_enabled) {
        if (!detect_profile(input, result.profile)) {
            return false;
        }
    }

    if (!transform_text_with_profile(input, learned_corrections,
                                     result.profile, result.text)) {
        return false;
    }
    return true;
}

bool LlmCleaner::detect_profile(const std::string& input,
                                std::string& profile) const {
    std::string response_text;
    if (!request_completion(input, config_.intent_detection_prompt,
                            response_text)) {
        return false;
    }
    if (!parse_profile_label(response_text, profile)) {
        profile = "general";
    }
    return true;
}

bool LlmCleaner::transform_text_with_profile(
    const std::string& input,
    const std::string& learned_corrections,
    const std::string& profile,
    std::string& output) const {
    std::string selected_profile = is_valid_profile(profile) ? profile : "general";
    std::string system_prompt = config_.pre_prompt;
    if (!learned_corrections.empty()) {
        system_prompt += "\n\n";
        system_prompt += learned_corrections;
    }
    system_prompt += "\n\nSelected profile: ";
    system_prompt += selected_profile;
    system_prompt += "\n\n";
    system_prompt += config_.profile_transform_prompt;

    return request_completion(input, system_prompt, output);
}

bool LlmCleaner::transform_text(const std::string& input,
                                const std::string& system_prompt,
                                std::string& output) const {
    return request_completion(input, system_prompt, output);
}

bool LlmCleaner::request_completion(const std::string& input,
                                    const std::string& system_prompt,
                                    std::string& output) const {
    if (!config_.enabled || input.empty()) return false;

    ParsedUrl url;
    if (!parse_url(config_.endpoint, url)) return false;

    HINTERNET session = WinHttpOpen(L"VoiceToText/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    WinHttpSetTimeouts(session, config_.timeout_ms, config_.timeout_ms,
                       config_.timeout_ms, config_.timeout_ms);

    HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", url.path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::string body = build_request_body(config_, system_prompt, input);
    std::wstring headers =
        L"Content-Type: application/json; charset=utf-8\r\n";

    BOOL ok = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.data(), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (ok) ok = WinHttpReceiveResponse(request, nullptr);

    std::string response;
    if (ok) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                            &status_size, WINHTTP_NO_HEADER_INDEX);
        ok = status_code >= 200 && status_code < 300;
    }

    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }

        std::vector<char> buffer(static_cast<size_t>(available));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            ok = FALSE;
            break;
        }
        response.append(buffer.data(), buffer.data() + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!ok || response.empty()) return false;
    return parse_cleaned_text(response, output);
}

} // namespace vtt
