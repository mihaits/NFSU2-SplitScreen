#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// SBC encoder for the DualShock 4's speaker (docs/input.md), written from the A2DP specification, appendix B. Only
// what the pad needs: 32 kHz, 16 blocks, 8 subbands, stereo mode, SNR allocation, bitpool 48. The input is mono and
// goes into both channels, as the speaker plays one. A frame is 128 samples -> kSbcFrameBytes.
constexpr int kSbcFrameSamples = 128, kSbcFrameBytes = 108;

class SbcEncoder {
public:
    // in: kSbcFrameSamples samples, -1..1
    void Encode(const float* in, uint8_t* out) {
        static const int kBlocks = 16, kBands = 8, kBitpool = 48;
        float s[16][8];
        for (int b = 0; b < kBlocks; ++b) Analyze(in + b * kBands, s[b]);
        uint8_t scf[8];
        for (int k = 0; k < kBands; ++k) {
            float top = 0;
            for (int b = 0; b < kBlocks; ++b) top = fmaxf(top, fabsf(s[b][k]));
            int v = 0;
            while (v < 15 && top >= static_cast<float>(2 << v)) ++v;
            scf[k] = static_cast<uint8_t>(v);
        }
        int bits[2][8];
        Allocate(scf, kBitpool, bits);

        memset(out, 0, kSbcFrameBytes);
        out[0] = 0x9C;
        out[1] = 0x7B;                    // 32 kHz, 16 blocks, stereo, SNR, 8 subbands
        out[2] = kBitpool;
        for (int i = 0; i < 16; ++i) out[4 + i / 2] |= scf[i % 8] << (i & 1 ? 0 : 4);
        uint8_t crc = 0x0F;               // over byte 1, the bitpool and the scale factors
        crc = Crc8(crc, out[1]);
        crc = Crc8(crc, out[2]);
        for (int i = 4; i < 12; ++i) crc = Crc8(crc, out[i]);
        out[3] = crc;

        int pos = 12 * 8;
        for (int b = 0; b < kBlocks; ++b)
            for (int ch = 0; ch < 2; ++ch)
                for (int k = 0; k < kBands; ++k) {
                    int n = bits[ch][k];
                    if (!n) continue;
                    int levels = (1 << n) - 1;
                    float scale = static_cast<float>(2 << scf[k]);
                    int q = static_cast<int>((s[b][k] / scale + 1) * levels * 0.5f);
                    q = q < 0 ? 0 : q > levels ? levels : q;
                    for (int i = n - 1; i >= 0; --i, ++pos)
                        if (q >> i & 1) out[pos >> 3] |= 0x80 >> (pos & 7);
                }
    }

private:
    float x_[80] = {};

    static uint8_t Crc8(uint8_t c, uint8_t v) {
        c ^= v;
        for (int i = 0; i < 8; ++i) c = c & 0x80 ? static_cast<uint8_t>(c << 1 ^ 0x1D) : static_cast<uint8_t>(c << 1);
        return c;
    }

    // The analysis window C[i]: the symmetric prototype p[i] = p[80 - i], negated in every other block of 16.
    static float Window(int i) {
        static const float kProto[41] = {
            0.00000000E+00f,  1.56575398E-04f,  3.43256425E-04f,  5.54620202E-04f,  8.23919506E-04f,  1.13992507E-03f,
            1.47640169E-03f,  1.78371725E-03f,  2.01182542E-03f,  2.10371989E-03f,  1.99454554E-03f,  1.61656283E-03f,
            9.02154502E-04f,  -1.78805361E-04f, -1.64973098E-03f, -3.49717454E-03f, -5.65949473E-03f, -8.02941163E-03f,
            -1.04584443E-02f, -1.27472335E-02f, -1.46525263E-02f, -1.59045603E-02f, -1.62208471E-02f, -1.53184106E-02f,
            -1.29371806E-02f, -8.85757540E-03f, -2.92408442E-03f, 4.91578024E-03f,  1.46404076E-02f,  2.61098752E-02f,
            3.90751381E-02f,  5.31873032E-02f,  6.79989431E-02f,  8.29847578E-02f,  9.75753918E-02f,  1.11196689E-01f,
            1.23264548E-01f,  1.33264415E-01f,  1.40753505E-01f,  1.45389847E-01f,  1.46955068E-01f};
        float p = kProto[i <= 40 ? i : 80 - i];
        return (i >> 4) & 1 ? -p : p;
    }

    void Analyze(const float* in, float* out) {
        static float cosine[8][16], window[80];
        static bool ready = false;
        if (!ready) {
            for (int k = 0; k < 8; ++k)
                for (int i = 0; i < 16; ++i) cosine[k][i] = cosf((2 * k + 1) * (i - 4) * 3.14159265358979f / 16);
            for (int i = 0; i < 80; ++i) window[i] = Window(i);
            ready = true;
        }
        memmove(x_ + 8, x_, 72 * sizeof(float));
        for (int i = 0; i < 8; ++i) x_[7 - i] = in[i] * 32768.0f;
        float y[16] = {};
        for (int i = 0; i < 80; ++i) y[i % 16] += window[i] * x_[i];
        for (int k = 0; k < 8; ++k) {
            float v = 0;
            for (int i = 0; i < 16; ++i) v += cosine[k][i] * y[i];
            out[k] = v;
        }
    }

    // Bit allocation with SNR needs (bitneed = scale factor), shared across both channels.
    static void Allocate(const uint8_t* scf, int bitpool, int bits[2][8]) {
        int top = 0;
        for (int k = 0; k < 8; ++k) top = scf[k] > top ? scf[k] : top;
        int slice = top + 1, count = 0, sliceCount = 0;
        do {
            --slice;
            count += sliceCount;
            sliceCount = 0;
            for (int k = 0; k < 8; ++k) {
                if (scf[k] > slice + 1 && scf[k] < slice + 16) sliceCount += 2;
                else if (scf[k] == slice + 1) sliceCount += 4;
            }
        } while (count + sliceCount < bitpool);
        if (count + sliceCount == bitpool) { count += sliceCount; --slice; }
        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < 8; ++k) {
                int b = scf[k] < slice + 2 ? 0 : scf[k] - slice;
                bits[ch][k] = b > 16 ? 16 : b;
            }
        int ch = 0, k = 0;
        auto next = [&] { if (ch) { ch = 0; ++k; } else ch = 1; };
        while (count < bitpool && k < 8) {
            if (bits[ch][k] >= 2 && bits[ch][k] < 16) { ++bits[ch][k]; ++count; }
            else if (scf[k] == slice + 1 && bitpool > count + 1) { bits[ch][k] = 2; count += 2; }
            next();
        }
        ch = k = 0;
        while (count < bitpool && k < 8) {
            if (bits[ch][k] < 16) { ++bits[ch][k]; ++count; }
            next();
        }
    }
};
