// Drives the production WinHTTP path of LlmCleaner against a local mock of
// an OpenAI-compatible server: the GET .../v1/models probe, the request
// layout each answer selects, primes, UTF-8 bodies and the time limit.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../src/llm_cleaner.cpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Request {
    std::string method;
    std::string path;
    std::string headers;
    std::string body;
};

// One listening socket on 127.0.0.1 with an ephemeral port. Every request
// is recorded; the answer depends on the settings below.
class MockServer {
public:
    // Empty: GET .../models answers 404.
    std::string models_owner;
    // Completions wait this long; negative: never answer (until stop()).
    int completion_delay_ms = 0;

    MockServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener_ != INVALID_SOCKET, "socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind failed");
        require(listen(listener_, 16) == 0, "listen failed");
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        accept_thread_ = std::thread([this]() { accept_loop(); });
    }
    ~MockServer() { stop(); }

    void stop() {
        if (stopped_.exchange(true)) return;
        closesocket(listener_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (SOCKET client : clients_) shutdown(client, SD_BOTH);
        }
        cv_.notify_all();
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }

    std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1/chat/completions";
    }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.clear();
    }

private:
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::atomic<bool> stopped_{false};
    std::thread accept_thread_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<SOCKET> clients_;
    std::vector<Request> requests_;

    void accept_loop() {
        for (;;) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) { closesocket(client); return; }
            clients_.push_back(client);
            workers_.emplace_back([this, client]() { serve(client); });
        }
    }

    static bool read_request(SOCKET client, std::string& buffer, Request& request) {
        size_t header_end;
        while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            char chunk[4096];
            const int got = recv(client, chunk, sizeof(chunk), 0);
            if (got <= 0) return false;
            buffer.append(chunk, chunk + got);
        }
        const std::string head = buffer.substr(0, header_end);
        buffer.erase(0, header_end + 4);
        const size_t first_space = head.find(' ');
        const size_t second_space = head.find(' ', first_space + 1);
        request.method = head.substr(0, first_space);
        request.path = head.substr(first_space + 1, second_space - first_space - 1);
        request.headers = head;
        size_t content_length = 0;
        std::string lower = head;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t field = lower.find("\r\ncontent-length:");
        if (field != std::string::npos)
            content_length = static_cast<size_t>(std::stoul(head.substr(field + 17)));
        while (buffer.size() < content_length) {
            char chunk[4096];
            const int got = recv(client, chunk, sizeof(chunk), 0);
            if (got <= 0) return false;
            buffer.append(chunk, chunk + got);
        }
        request.body = buffer.substr(0, content_length);
        buffer.erase(0, content_length);
        return true;
    }

    static void send_all(SOCKET client, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const int n = send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    static std::string response(int status, const std::string& body) {
        return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") +
               "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\n\r\n" + body;
    }

    void serve(SOCKET client) {
        std::string buffer;
        Request request;
        while (read_request(client, buffer, request)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
            }
            if (request.method == "GET") {
                if (models_owner.empty()) {
                    send_all(client, response(404, R"({"error":"not found"})"));
                } else {
                    send_all(client, response(200, nlohmann::json{{"object", "list"}, {"data",
                        nlohmann::json::array({{{"id", "mock"}, {"object", "model"},
                                                {"owned_by", models_owner}}})}}.dump()));
                }
                continue;
            }
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (completion_delay_ms < 0) {
                    cv_.wait(lock, [this]() { return stopped_.load(); });
                } else if (completion_delay_ms > 0) {
                    cv_.wait_for(lock, std::chrono::milliseconds(completion_delay_ms),
                                 [this]() { return stopped_.load(); });
                }
                if (stopped_) break;
            }
            // Echo the last message, so the UTF-8 round trip is visible.
            std::string last;
            try {
                last = nlohmann::json::parse(request.body).at("messages").back().at("content").get<std::string>();
            } catch (const nlohmann::json::exception&) {
            }
            const std::string body = nlohmann::json{
                {"choices", nlohmann::json::array({{{"finish_reason", "stop"},
                    {"message", {{"role", "assistant"}, {"content", u8"Svar: " + last}}}}})},
                {"usage", {{"prompt_tokens", 10}, {"prompt_tokens_details", {{"cached_tokens", 4}}}}}
            }.dump();
            send_all(client, response(200, body));
        }
        closesocket(client);
    }
};

std::vector<std::string> g_log;
std::mutex g_log_mutex;
void collect_log(const char* line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log.emplace_back(line);
    std::cout << "  log: " << line << '\n';
}

vtt::LlmCleanerConfig settings_for(const MockServer& server, bool cache_layout) {
    vtt::LlmCleanerConfig settings;
    settings.enabled = true;
    settings.endpoint = server.endpoint();
    settings.model = "mock";
    settings.timeout_ms = 5000;
    settings.cache_layout = cache_layout;
    settings.log_label = "test";
    return settings;
}

size_t count(const std::vector<Request>& requests, const std::string& method) {
    size_t n = 0;
    for (const auto& request : requests) n += request.method == method ? 1 : 0;
    return n;
}

const std::string kDictation = u8"Ändra inte filen på måndag, åtta över tre.";
const std::string kPrompt = u8"Skriv om texten tydligt.";

// TensorFold answers the probe: the rewrite carries the fixed exchange, and
// prime() sends three one-token requests with exactly that prefix.
void tensorfold_server_gets_cache_layout_and_primes() {
    MockServer server;
    server.models_owner = "tensorfold";
    vtt::LlmCleaner cleaner;
    cleaner.configure(settings_for(server, true));
    std::string output;
    require(cleaner.transform_text(kDictation, kPrompt, output), "rewrite against TensorFold failed: " +
            vtt::LlmCleaner::last_failure());
    require(output == u8"Svar: " + kDictation, "UTF-8 text did not survive the round trip: " + output);
    auto requests = server.requests();
    require(requests.size() == 2 && requests[0].method == "GET" && requests[0].path == "/v1/models" &&
            requests[1].method == "POST" && requests[1].path == "/v1/chat/completions",
            "expected the model-list probe followed by one completion");
    const auto body = nlohmann::json::parse(requests[1].body);
    const auto& messages = body.at("messages");
    require(messages.size() == 4 && messages[0].at("content") == vtt::LlmCleaner::rewrite_system_prompt(kPrompt) &&
            messages[1].at("content") == vtt::CACHE_LAYOUT_USER &&
            messages[2].at("content") == vtt::CACHE_LAYOUT_ASSISTANT && messages[3].at("content") == kDictation,
            "a TensorFold server must get the cacheable layout");
    require(requests[1].headers.find("application/json; charset=utf-8") != std::string::npos,
            "the completion must be sent as UTF-8 JSON");

    server.clear();
    require(cleaner.prime(vtt::LlmCleaner::rewrite_system_prompt(kPrompt)), "prime failed");
    requests = server.requests();
    require(count(requests, "GET") == 0 && count(requests, "POST") == 3,
            "a prime is three completions, and the probe answer is remembered");
    const auto prefix = nlohmann::json(messages.begin(), messages.begin() + 3).dump();
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto prime = nlohmann::json::parse(requests[i].body);
        const auto& sent = prime.at("messages");
        require(prime.at("max_tokens") == 1, "a prime asks for one token");
        if (i == 0) {
            require(nlohmann::json(sent.begin(), sent.end()).dump() ==
                    nlohmann::json(messages.begin(), messages.begin() + 2).dump(),
                    "the first prime sends the prompt and the fixed user turn");
        } else {
            require(sent.size() == 4 && nlohmann::json(sent.begin(), sent.begin() + 3).dump() == prefix,
                    "later primes send the request's exact prefix");
        }
    }
    std::cout << "PASS: TensorFold probe -> cacheable layout, 3 primes, UTF-8 round trip\n";
}

// Any other owner, or no model list at all: the plain body, no primes.
void other_server_gets_plain_layout(const std::string& owner) {
    MockServer server;
    server.models_owner = owner;
    vtt::LlmCleaner cleaner;
    cleaner.configure(settings_for(server, true));
    std::string output;
    require(cleaner.transform_text(kDictation, kPrompt, output), "rewrite against another server failed");
    auto requests = server.requests();
    require(requests.size() == 2 && requests[0].method == "GET" && requests[0].path == "/v1/models",
            "the model list must be asked once");
    const auto body = nlohmann::json::parse(requests[1].body);
    const auto& messages = body.at("messages");
    require(messages.size() == 2 && messages[1].at("content") == kDictation,
            "another server must get the plain layout");
    server.clear();
    require(!cleaner.prime(vtt::LlmCleaner::rewrite_system_prompt(kPrompt)) && server.requests().empty(),
            "another server is never primed");
    std::cout << "PASS: " << (owner.empty() ? std::string("404 model list") : "owned_by " + owner)
              << " -> plain layout, no primes\n";
}

// History and the dictation window: never a probe.
void cache_layout_off_sends_no_probe() {
    MockServer server;
    server.models_owner = "tensorfold";
    vtt::LlmCleaner cleaner;
    cleaner.configure(settings_for(server, false));
    std::string output;
    require(cleaner.transform_text(kDictation, kPrompt, output), "plain rewrite failed");
    const auto requests = server.requests();
    require(requests.size() == 1 && requests[0].method == "POST" &&
            nlohmann::json::parse(requests[0].body).at("messages").size() == 2,
            "with the cache layout off only the plain completion is sent");
    std::cout << "PASS: cache layout off -> no probe, plain layout\n";
}

void api_key_is_sent() {
    MockServer server;
    vtt::LlmCleanerConfig settings = settings_for(server, false);
    settings.api_key = "test-key-123";
    vtt::LlmCleaner cleaner;
    cleaner.configure(settings);
    std::string output;
    require(cleaner.transform_text(kDictation, kPrompt, output), "rewrite with a key failed");
    const auto requests = server.requests();
    require(requests.size() == 1 &&
            requests[0].headers.find("Authorization: Bearer test-key-123") != std::string::npos,
            "the API key must be sent as a bearer token");
    std::cout << "PASS: API key sent as bearer token\n";
}

// A server that never answers: the call returns after the time limit.
void hanging_server_respects_the_time_limit() {
    MockServer server;
    server.models_owner = "tensorfold";
    server.completion_delay_ms = -1;
    vtt::LlmCleanerConfig settings = settings_for(server, true);
    settings.timeout_ms = 1500;
    vtt::LlmCleaner cleaner;
    cleaner.configure(settings);
    std::string output;
    const auto started = std::chrono::steady_clock::now();
    const bool ok = cleaner.transform_text(kDictation, kPrompt, output);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    require(!ok && vtt::LlmCleaner::last_failure() == "request_failed",
            "a request without an answer must fail as request_failed");
    // Measured on a Windows 11 test machine (build 26300): WinHTTP ended
    // every time limit on whole steps of about 4 s (1500 ms after about
    // 4000 ms), so the bound allows for that.
    require(ms >= 1300 && ms < 1500 + 4500, "the time limit was not honoured: " + std::to_string(ms) + " ms");
    std::cout << "PASS: hanging server -> request_failed after " << ms << " ms (limit 1500)\n";
    server.stop();
}

// Nothing listens: the request fails without waiting for the time limit.
void refused_connection_fails_fast() {
    unsigned short port;
    {
        MockServer server;
        port = static_cast<unsigned short>(std::stoi(server.endpoint().substr(17)));
    }
    vtt::LlmCleanerConfig settings;
    settings.enabled = true;
    settings.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
    settings.timeout_ms = 5000;
    settings.cache_layout = true;
    vtt::LlmCleaner cleaner;
    cleaner.configure(settings);
    std::string output;
    const auto started = std::chrono::steady_clock::now();
    require(!cleaner.transform_text(kDictation, kPrompt, output), "a refused request must fail");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    // Measured on a Windows 11 test machine (build 26300): each refused
    // exchange took about 2 s, once for the model-list probe and once for
    // the completion.
    require(ms < 6000, "a refused connection took " + std::to_string(ms) + " ms");
    std::cout << "PASS: refused connection -> fails in " << ms << " ms\n";
}

} // namespace

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    vtt::LlmCleaner::set_log_sink(&collect_log);
    try {
        tensorfold_server_gets_cache_layout_and_primes();
        other_server_gets_plain_layout("llamacpp");
        other_server_gets_plain_layout("");
        cache_layout_off_sends_no_probe();
        api_key_is_sent();
        hanging_server_respects_the_time_limit();
        refused_connection_fails_fast();
        std::cout << "LLM WinHTTP tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LLM WinHTTP test failed: " << error.what() << '\n';
        return 1;
    }
}
