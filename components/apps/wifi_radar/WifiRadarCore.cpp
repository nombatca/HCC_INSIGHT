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
        for (int k = 0; k < w; ++k) {
            int idx = (_head - 1 - k) % kHistMax;
            if (idx < 0) idx += kHistMax;
            double v = _hist[idx];
            sum += v;
            sum2 += v * v;
        }
        double mean = sum / w;
        double var = sum2 / w - mean * mean;
        if (var < 0) var = 0;
        _activity = (float)std::sqrt(var);
    } else {
        _activity = 0;
    }

    // Quyết định chuyển động + giữ trạng thái.
    if (_activity >= _cfg.threshold) {
        _motion = true;
        _last_motion_ms = t_ms;
    } else if (_motion && (t_ms - _last_motion_ms) > _cfg.hold_ms) {
        _motion = false;
    }
}

int WifiRadar::level() const
{
    if (_cfg.threshold <= 0) return 0;
    // 0 khi activity=0, 100 khi activity=2*threshold.
    float r = _activity / (_cfg.threshold * 2.0f);
    int v = (int)(r * 100.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
