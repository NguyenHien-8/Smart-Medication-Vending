#pragma once

#include <cstddef>
#include <cstdint>

// SmartMediVend INMP441-only, bounded PCM conditioning. Pure C++ to permit
// host-side boundary tests. The microphone supplies 24-bit samples in a
// 32-bit I2S slot. Keep 12 dB of headroom here; WebRTC AFE owns AGC/NS so
// capture does not pump its gain independently on every 10-ms frame.
namespace smv {
class MicProcessor {
public:
    struct FrameStats {
        uint32_t mean_abs_before = 0;
        uint32_t mean_abs_after = 0;
        uint32_t peak_before = 0;
        uint32_t clipped_input = 0;
        uint32_t clipped_output = 0;
        uint16_t gain_q8 = 256;
    };

    void Reset() { dc_estimate_ = 0; }

    FrameStats Process(const int32_t* raw, int16_t* pcm, size_t samples) {
        FrameStats stats;
        if (raw == nullptr || pcm == nullptr || samples == 0) {
            return stats;
        }
        uint64_t sum_abs = 0;
        for (size_t i = 0; i < samples; ++i) {
            const int32_t sample = raw[i] >> 14;
            // Fixed-point DC removal (~10 Hz at 16 kHz); no frame-edge jump.
            dc_estimate_ += (sample - dc_estimate_) / 256;
            const int32_t filtered = sample - dc_estimate_;
            const int32_t clean = Clamp16(filtered);
            if (filtered != clean)
                ++stats.clipped_input;
            pcm[i] = static_cast<int16_t>(clean);
            const uint32_t magnitude = Abs16(clean);
            sum_abs += magnitude;
            if (magnitude > stats.peak_before)
                stats.peak_before = magnitude;
        }
        stats.mean_abs_before = static_cast<uint32_t>(sum_abs / samples);
        stats.mean_abs_after = stats.mean_abs_before;
        return stats;
    }

private:
    static int32_t Clamp16(int32_t v) {
        if (v > 32767)
            return 32767;
        if (v < -32768)
            return -32768;
        return v;
    }
    static uint32_t Abs16(int32_t v) { return static_cast<uint32_t>(v < 0 ? -v : v); }
    int32_t dc_estimate_ = 0;
};
}  // namespace smv
