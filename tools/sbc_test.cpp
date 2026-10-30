// Checks src/SbcEncoder.h: encodes 4 s of test tones to an .sbc file and the same input to a raw float32 file. Decode
// the .sbc with ffmpeg and compare with sbc_check.py.
//
// Build (32-bit VS prompt): cl /nologo /O2 /EHsc /Fo:build\ /Fe:build\sbc_test.exe tools\sbc_test.cpp
#include <cstdio>
#include <vector>

#include "../src/SbcEncoder.h"

int main(int argc, char** argv) {
    const char* base = argc > 1 ? argv[1] : "build/sbc_test";
    constexpr int kRate = 32000, kSeconds = 4;
    std::vector<float> in(kRate * kSeconds);
    for (size_t i = 0; i < in.size(); ++i) {
        float t = static_cast<float>(i) / kRate;
        float sweep = 0.25f * sinf(6.2831853f * (200 + 900 * t) * t);          // 200 Hz up to 7.4 kHz
        in[i] = 0.25f * sinf(6.2831853f * 440 * t) + sweep;
    }
    char path[260];
    snprintf(path, sizeof(path), "%s.raw", base);
    FILE* raw = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s.sbc", base);
    FILE* sbc = fopen(path, "wb");
    if (!raw || !sbc) return 1;
    fwrite(in.data(), sizeof(float), in.size(), raw);
    SbcEncoder enc;
    uint8_t frame[kSbcFrameBytes];
    for (size_t i = 0; i + kSbcFrameSamples <= in.size(); i += kSbcFrameSamples) {
        enc.Encode(&in[i], frame);
        fwrite(frame, 1, sizeof(frame), sbc);
    }
    fclose(raw);
    fclose(sbc);
    printf("%zu frames\n", in.size() / kSbcFrameSamples);
    return 0;
}
