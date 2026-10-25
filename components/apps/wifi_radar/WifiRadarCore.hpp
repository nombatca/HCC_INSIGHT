#pragma once

#include <stdint.h>
#include <stddef.h>

// Bộ dò "có chuyển động" dựa trên BIẾN ĐỘNG RSSI (thuần C++, không phụ thuộc ESP-IDF
// nên test được trên host). Nguyên lý: người di chuyển làm nhiễu sóng WiFi -> RSSI
// dao động mạnh hơn mức nhiễu nền. Đo độ lệch chuẩn trong cửa sổ ngắn (activity);
// activity vượt ngưỡng -> báo chuyển động (giữ trạng thái thêm hold_ms cho đỡ chớp).
class WifiRadar {
public:
    static constexpr int kHistMax = 160;   // số mẫu lưu để vẽ đồ thị

    struct Config {
        int   act_win    = 16;     // số mẫu tính activity (độ lệch chuẩn / đỉnh-đỉnh)
        float ema_alpha  = 0.05f;  // hệ số EMA cho baseline (nhỏ = baseline chậm)
        float threshold  = 1.4f;   // ngưỡng activity (dB) báo chuyển động
        uint32_t hold_ms = 1500;   // giữ trạng thái "có chuyển động" sau lần kích cuối
    };

    void reset();
    void setConfig(const Config &c) { _cfg = c; }
    void setThreshold(float t) { _cfg.threshold = t; }
    const Config &config() const { return _cfg; }

    // Nạp 1 mẫu RSSI (dBm, âm) tại thời điểm t_ms. Tự cập nhật mọi chỉ số.
    void push(int rssi, uint32_t t_ms);

    float rssi() const { return _rssi; }
    float baseline() const { return _baseline; }
    float activity() const { return _activity; }
    float p2p() const { return _p2p; }          // đỉnh-đỉnh (max-min) trong cửa sổ
    int   wmin() const { return _wmin; }
    int   wmax() const { return _wmax; }
    bool  motion() const { return _motion; }
    // Mức 0..100 để vẽ gauge (activity so với ngưỡng).
    int   level() const;

    int count() const { return _count; }
    // Lịch sử RSSI (vòng): [0] là cũ nhất trong số đang giữ.
    int histSize() const { return _count < kHistMax ? _count : kHistMax; }
    int histAt(int i) const;  // i: 0..histSize()-1 (cũ -> mới)

private:
    Config _cfg;
    int   _hist[kHistMax] = {0};
    int   _head = 0;        // vị trí ghi tiếp theo
    int   _count = 0;       // tổng số mẫu đã nạp
    float _rssi = 0;
    float _baseline = 0;
    float _activity = 0;
    float _p2p = 0;
    int   _wmin = 0, _wmax = 0;
    bool  _motion = false;
    uint32_t _last_motion_ms = 0;
    bool  _primed = false;
};
