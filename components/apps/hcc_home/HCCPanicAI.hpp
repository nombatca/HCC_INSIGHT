#pragma once

#include <string>
#include "HCCPanicTypes.hpp"

// Gọi API AI (raw HTTPS) để phân tích panic đã parse. Hỗ trợ Anthropic (Claude)
// và OpenAI (GPT). Cấu hình đọc từ /panic/ai_config.json, ví dụ:
//   { "api_key": "sk-...", "provider": "openai", "model": "gpt-4o-mini" }
//   { "api_key": "sk-ant-...", "provider": "anthropic", "model": "claude-opus-5" }
// Nếu thiếu "provider": tự đoán theo key (sk-ant... = anthropic, còn lại = openai).
// Kết quả luôn mang source_tier = AI_INFERENCE.
class HCCPanicAI {
public:
    static PanicResult analyze(const ParsedPanic &parsed);

    // Phân tích panic từ ẢNH (AI vision đọc ảnh). mime vd "image/jpeg".
    static PanicResult analyzeImage(const std::string &img_bytes, const std::string &mime);

    // Lưu API key vào /panic/ai_config.json (giữ nguyên model/provider nếu có).
    static bool saveApiKey(const std::string &key);
    // True nếu ai_config.json đã có api_key.
    static bool hasApiKey();

private:
    static bool loadConfig(std::string &api_key, std::string &provider,
                           std::string &model, std::string &url);
    static std::string buildUserContent(const ParsedPanic &parsed);
    static PanicResult parseAiText(const std::string &text);
    // Gửi 1 request (body JSON đã dựng) tới provider, parse ra 4 mục.
    static PanicResult sendAndParse(const std::string &provider, const std::string &url,
                                    const std::string &api_key, const std::string &body_json);
};
