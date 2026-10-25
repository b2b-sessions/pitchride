// B2B: renders the BEAT FX (src/effects/backends/builtin/b2bbeatfxdsp.h) on a
// test loop, one WAV per effect, to listen to and tune them outside the app.
//
//   g++ -O2 -std=c++20 -Ilib/reverb -Isrc/effects/backends/builtin \
//       tools/b2b_beatfx_render.cpp lib/reverb/Reverb.cc -o /tmp/beatfx_render
//   /tmp/beatfx_render OUT_DIR [input.raw]
//
// Each file: 1 s dry, the effect ON for 3 s, OFF for 3 s (tails). 120 BPM.
// input.raw (optional): interleaved stereo float32 at 44.1 kHz.
#include <xmmintrin.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "b2bbeatfxdsp.h"

namespace {
constexpr double kRate = 44100;
constexpr double kBpm = 120;
const char* kNames[] = {"01_delay", "02_echo", "03_pingpong", "04_spiral", "05_helix", "06_reverb",
        "07_flanger", "08_phaser", "09_filter", "10_triplet_filter", "11_trans", "12_roll",
        "13_triplet_roll", "14_mobius"};
const double kBeats[] = {0.75, 0.75, 0.5, 0.5, 1, 60, 4, 4, 2, 4.0 / 3, 0.25, 0.25, 1.0 / 3, 2};

void writeWav(const std::string& path, const std::vector<float>& s) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&f](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(s.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<uint32_t>(kRate));
    u32(static_cast<uint32_t>(kRate * 4));
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    for (float v : s) {
        const auto i = static_cast<int16_t>(std::clamp(v, -1.f, 1.f) * 32767);
        u16(static_cast<uint16_t>(i));
    }
}

// A simple loop: kick on the beat, hats on the off-beat, a held chord.
std::vector<float> testLoop(int frames) {
    std::vector<float> s(frames * 2);
    const double beat = 60.0 / kBpm * kRate;
    double kp = 0;
    for (int i = 0; i < frames; ++i) {
        const double inBeat = std::fmod(i, beat);
        const double tk = inBeat / kRate;
        const double kf = 50 + 120 * std::exp(-tk * 30);
        kp += kf / kRate;
        const double kick = std::sin(2 * M_PI * kp) * std::exp(-tk * 7) * 0.6;
        const double th = std::fmod(i + beat / 2, beat) / kRate;
        const double hat = ((std::rand() / double(RAND_MAX)) * 2 - 1) * std::exp(-th * 60) * 0.15;
        const double t = i / kRate;
        const double chord = 0.07 * (std::sin(2 * M_PI * 220 * t) + std::sin(2 * M_PI * 277.2 * t) +
                                            std::sin(2 * M_PI * 329.6 * t));
        s[2 * i] = static_cast<float>(kick + hat + chord);
        s[2 * i + 1] = static_cast<float>(kick + hat * 0.8 + chord);
    }
    return s;
}
} // namespace

int main(int argc, char** argv) {
    _mm_setcsr(_mm_getcsr() | 0x8040); // flush denormals, as the engine does
    const std::string out = argc > 1 ? argv[1] : ".";
    const int frames = static_cast<int>(7 * kRate);
    std::vector<float> input;
    if (argc > 2) {
        std::ifstream f(argv[2], std::ios::binary);
        input.resize(frames * 2);
        f.read(reinterpret_cast<char*>(input.data()), input.size() * 4);
    } else {
        input = testLoop(frames);
    }
    writeWav(out + "/00_dry.wav", input);
    const int buffer = 1024;
    const double beatFrames = 60.0 / kBpm * kRate;
    for (int fx = 0; fx < b2bfx::kFxCount; ++fx) {
        b2bfx::Dsp dsp(kRate);
        std::vector<float> o(input.size());
        b2bfx::Params p;
        p.fx = fx;
        p.beats = kBeats[fx];
        p.level = 0.8;
        double maxJump = 0;
        float prevL = 0;
        for (int at = 0; at < frames; at += buffer) {
            const int n = std::min(buffer, frames - at);
            p.on = at >= kRate * 1 && at < kRate * 4;
            b2bfx::Timing t;
            t.beatFrames = beatFrames;
            t.hasFraction = true;
            t.fractionAtEnd = std::fmod((at + n) / beatFrames, 1.0);
            dsp.process(&input[2 * at], &o[2 * at], n, p, t);
            for (int i = 0; i < n; ++i) {
                const float v = o[2 * (at + i)];
                maxJump = std::max<double>(maxJump, std::fabs(v - prevL));
                prevL = v;
            }
        }
        // Loudness of the tail after OFF (4 .. 5 s) against the dry.
        double tail = 0, dry = 0;
        for (int i = static_cast<int>(4.2 * kRate); i < static_cast<int>(5.2 * kRate); ++i) {
            tail += (o[2 * i] - input[2 * i]) * (o[2 * i] - input[2 * i]);
            dry += input[2 * i] * input[2 * i];
        }
        double on = 0;
        for (int i = static_cast<int>(1.5 * kRate); i < static_cast<int>(3.5 * kRate); ++i) {
            on += (o[2 * i] - input[2 * i]) * (o[2 * i] - input[2 * i]);
        }
        bool bad = false;
        for (float v : o) {
            bad |= !std::isfinite(v);
        }
        std::printf("%-18s effect %6.1f dB   tail after OFF %6.1f dB   biggest step %.3f%s\n",
                kNames[fx],
                10 * std::log10(on / dry + 1e-12),
                10 * std::log10(tail / dry + 1e-12),
                maxJump,
                bad ? "  NaN!" : "");
        writeWav(out + "/" + kNames[fx] + ".wav", o);
    }
    return 0;
}
