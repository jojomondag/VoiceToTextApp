#pragma once

#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace vtt {

struct GemmaModelInfo {
    std::string id;
    std::string repo;
    std::string filename;
    std::string local_path;
    unsigned long long expected_size_bytes = 0;
};

const std::vector<GemmaModelInfo>& gemma_models();
const GemmaModelInfo* find_gemma_model(const std::string& id);
const GemmaModelInfo& recommended_gemma_model();
std::string recommend_gemma_model_for_hardware();
std::string gemma_download_url_for_id(const std::string& id);
std::string normalize_gemma_model_id(const std::string& id);

class GemmaRuntime {
public:
    ~GemmaRuntime();

    bool start(const std::string& llama_server_path,
               const std::string& model_path,
               const std::string& compute_mode,
               int port,
               std::string& error);
    void stop();
    bool running() const;

private:
#ifdef _WIN32
    PROCESS_INFORMATION process_ = {};
#else
    pid_t process_ = 0;
#endif
};

} // namespace vtt
