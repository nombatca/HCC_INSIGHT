/*
 * HCCXiaozhi: client trợ lý giọng nói Xiaozhi AI (giao thức của dự án mã nguồn mở xiaozhi-esp32,
 * https://github.com/78/xiaozhi-esp32 - docs/websocket.md), chạy trên HCC INSIGHT.
 *
 * Luồng:
 *   1. POST thông tin thiết bị tới máy chủ OTA (mặc định https://api.tenclass.net/xiaozhi/ota/).
 *      Lần đầu máy chủ trả mã kích hoạt 6 số: vào xiaozhi.me -> Thêm thiết bị -> nhập mã.
 *      Đã kích hoạt: máy chủ trả địa chỉ WebSocket + token (lưu NVS "hcc_xz").
 *   2. Bấm nói: mở WebSocket, gửi "hello", nhận "hello" (session_id), gửi listen start (mode auto,
 *      máy chủ tự phát hiện hết câu), truyền micro dạng Opus 16 kHz mono khung 60 ms.
 *   3. Máy chủ trả stt (câu người nói), llm (cảm xúc), tts start / sentence_start / stop và
 *      các khung Opus để phát ra loa. Hết tts thì tự nghe tiếp (bán song công: không thu khi đang nói).
 *
 * Không làm: từ đánh thức (wake word), MCP / điều khiển thiết bị, nâng cấp firmware qua OTA của Xiaozhi.
 */
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace HCCXiaozhi {

enum class State : uint8_t {
    Off,          // app đóng
    Checking,     // đang hỏi máy chủ OTA
    Activating,   // chờ nhập mã kích hoạt trên xiaozhi.me
    Ready,        // sẵn sàng, chưa trò chuyện
    Connecting,   // đang mở WebSocket / chờ hello
    Listening,    // đang thu giọng nói
    Speaking,     // trợ lý đang trả lời
    Error,
};

struct ChatLine {
    bool user;          // true: người nói (stt), false: trợ lý (tts)
    std::string text;
};

// Gọi khi mở / đóng app. start() chạy kiểm tra kích hoạt ở nền.
void start(void);
void stop(void);

// Nút chính: sẵn sàng -> bắt đầu trò chuyện; đang nghe -> kết thúc; đang nói -> ngắt lời và nghe tiếp;
// lỗi -> thử lại.
void talk(void);
// Kết thúc phiên trò chuyện (đóng WebSocket).
void endSession(void);
// Xoá token đã lưu và kiểm tra lại với máy chủ (dùng khi đổi tài khoản / máy chủ).
void resetActivation(void);

std::string otaUrl(void);
void setOtaUrl(const std::string &url);  // rỗng = máy chủ mặc định

State state(void);
const char *stateText(State st);
std::string activationCode(void);
std::string activationMessage(void);
std::string emotion(void);
std::string lastError(void);
std::string deviceId(void);

// Lịch sử trò chuyện (tối đa 40 dòng). version tăng mỗi khi có thay đổi.
uint32_t chatVersion(void);
std::vector<ChatLine> chat(void);
void clearChat(void);

// Cho hoạt hình nhân vật: mức âm 0..1000 của tiếng trả lời đang phát / micro đang thu,
// thinking = đã nghe xong câu hỏi, đang chờ trợ lý trả lời.
int speakLevel(void);
int micLevel(void);
bool thinking(void);

int volume(void);
void setVolume(int volume);

}  // namespace HCCXiaozhi
