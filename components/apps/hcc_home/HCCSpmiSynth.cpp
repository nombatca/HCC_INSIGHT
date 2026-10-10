#include "HCCSpmiSynth.hpp"

namespace HCCSpmiSynth {

void Synth::put(int c, int d)
{
    const size_t byte = _n >> 2;
    if (byte >= _buf.size()) {
        _buf.push_back(0);
    }
    _buf[byte] |= (uint8_t)(((c & 1) | ((d & 1) << 1)) << (2 * (_n & 3)));
    ++_n;
}

void Synth::level(int sclk, int sdata, int n)
{
    for (int i = 0; i < n; ++i) {
        put(sclk, sdata);
    }
}

void Synth::ssc()
{
    level(0, 1, _h);
    level(0, 0, _h);
}

void Synth::bit(int b)
{
    level(1, b, _h);  // cạnh lên: master đặt bit
    level(0, b, _h);  // cạnh xuống: bên nhận lấy mẫu
}

void Synth::frame(uint32_t v, int nbits, bool bad_parity)
{
    int ones = 0;
    for (int i = nbits - 1; i >= 0; --i) {
        const int b = (v >> i) & 1;
        ones += b;
        bit(b);
    }
    int p = (ones & 1) ? 0 : 1;  // parity lẻ: tổng số bit 1 (kể cả P) là số lẻ
    bit(bad_parity ? !p : p);
}

void Synth::park()
{
    level(1, 0, _h);
    level(0, 0, _h);
}

void Synth::command(uint8_t sid, uint8_t cmd, bool bad_cmd)
{
    ssc();
    frame(((uint32_t)(sid & 0xF) << 8) | cmd, 12, bad_cmd);
}

void Synth::reg0Write(uint8_t sid, uint8_t v7)
{
    command(sid, 0x80 | (v7 & 0x7F));
    park();
}

void Synth::regWrite(uint8_t sid, uint8_t addr5, uint8_t data, bool bad_data)
{
    command(sid, 0x40 | (addr5 & 0x1F));
    frame(data, 8, bad_data);
    park();
}

void Synth::regRead(uint8_t sid, uint8_t addr5, uint8_t data, bool bad_data)
{
    command(sid, 0x60 | (addr5 & 0x1F));
    park();
    frame(data, 8, bad_data);
    park();
}

void Synth::extWrite(uint8_t sid, uint8_t addr, const uint8_t *d, int n)
{
    command(sid, 0x00 | ((n - 1) & 0x0F));
    frame(addr, 8);
    for (int i = 0; i < n; ++i) {
        frame(d[i], 8);
    }
    park();
}

void Synth::extRead(uint8_t sid, uint8_t addr, const uint8_t *d, int n)
{
    command(sid, 0x20 | ((n - 1) & 0x0F));
    frame(addr, 8);
    park();
    for (int i = 0; i < n; ++i) {
        frame(d[i], 8);
    }
    park();
}

void Synth::extLongWrite(uint8_t sid, uint16_t addr, const uint8_t *d, int n)
{
    command(sid, 0x30 | ((n - 1) & 0x07));
    frame(addr >> 8, 8);
    frame(addr & 0xFF, 8);
    for (int i = 0; i < n; ++i) {
        frame(d[i], 8);
    }
    park();
}

void Synth::extLongRead(uint8_t sid, uint16_t addr, const uint8_t *d, int n)
{
    command(sid, 0x38 | ((n - 1) & 0x07));
    frame(addr >> 8, 8);
    frame(addr & 0xFF, 8);
    park();
    for (int i = 0; i < n; ++i) {
        frame(d[i], 8);
    }
    park();
}

std::vector<uint8_t> Synth::packedSwapped() const
{
    std::vector<uint8_t> o(_buf.size());
    for (size_t i = 0; i < _buf.size(); ++i) {
        const uint8_t v = _buf[i];
        o[i] = (uint8_t)(((v & 0x55) << 1) | ((v & 0xAA) >> 1));
    }
    return o;
}

}  // namespace HCCSpmiSynth
