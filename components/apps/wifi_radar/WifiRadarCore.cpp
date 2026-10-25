#include "WifiRadarCore.hpp"

#include <cmath>

void WifiRadar::reset()
{
    _head = 0;
    _count = 0;
    _rssi = 0;
    _baseline = 0;
    _activity = 0;
    _motion = false;
    _last_motion_ms = 0;
    _primed = false;
}

int WifiRadar::histAt(int i) const
{
    int n = histSize();
    if (i < 0 || i >= n) return 0;
    // mẫu cũ nhất nằm ở (_head - n) mod kHistMax.
    int start = (_head - n) % kHistMax;
    if (start < 0) start += kHistMax;
    return _hist[(start + i) % kHistMax];
}

void WifiRadar::push(int rssi, uint32_t t_ms)
{
    // RSSI hợp lệ thường -30..-95 dBm; 0 nghĩa là chưa đọc được -> bỏ.
    if (rssi == 0) return;
    _rssi = (float)rssi;

    _hist[_head] = rssi;
    _head = (_head + 1) % kHistMax;
    ++_count;

    if (!_primed) {
        _baseline = _rssi;
        _primed = true;
    } else {
        _baseline += _cfg.ema_alpha * (_rssi - _baseline);
    }

    // activity = độ lệch chuẩn của act_win mẫu gần nhất.
    int n = histSize();
    int w = _cfg.act_win;
    if (w > n) w = n;
    if (w >= 2) {
        double sum = 0, sum2 = 0;
        int mn = 127, mx = -127;
        for (int k = 0; k < w; ++k) {
            int idx = (_head - 1 - k) % kHistMax;
            if (idx < 0) idx += kHistMax;
            int iv = _hist[idx];
            double v = iv;
            sum += v;
            sum2 += v * v;
            if (iv < mn) mn = iv;
            if (iv > mx) mx = iv;
        }
        double mean = sum / w;
        double var = sum2 / w - mean * mean;
        if (var < 0) var = 0;
        _activity = (float)std::sqrt(var);
        _wmin = mn; _wmax = mx;
        _p2p = (float)(mx - mn);
    } else {
        _activity = 0; _p2p = 0; _wmin = _wmax = (int)_rssi;
    }

    // Quyết định chuyển động: std vượt ngưỡng HOẶC đỉnh-đỉnh vượt ~2.2x ngưỡng
    // (bắt được 1 cú dao động nhanh dù std chưa kịp lên). Giữ trạng thái hold_ms.
    bool trig = (_activity >= _cfg.threshold) || (_p2p >= _cfg.threshold * 2.2f);
    if (trig) {
        _motion = true;
        _last_motion_ms = t_ms;
    } else if (_motion && (t_ms - _last_motion_ms) > _cfg.hold_ms) {
        _motion = false;
    }
}

int WifiRadar::level() const
{
    if (_cfg.threshold <= 0) return 0;
    // dùng chỉ số mạnh hơn giữa std và (đỉnh-đỉnh / 2.2).
    float eff = _activity;
    float eff2 = _p2p / 2.2f;
    if (eff2 > eff) eff = eff2;
    float r = eff / (_cfg.threshold * 2.0f);
    int v = (int)(r * 100.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
