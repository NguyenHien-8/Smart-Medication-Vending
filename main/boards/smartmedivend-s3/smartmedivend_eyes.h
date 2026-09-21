#pragma once

#include <stdint.h>

// Header-only, allocation-free eye motion and RGB565 drawing. Kept independent
// of ESP-IDF and LVGL so the animation can be host-tested deterministically.
namespace smartmedivend_eyes {

constexpr int kScreenW = 240;
constexpr int kScreenH = 320;
constexpr int kSprite = 54;
constexpr int kCenterY = 142;
constexpr int kCentersX[2] = {87, 151};
static_assert(kCentersX[0] - kSprite / 2 >= 0 &&
              kCentersX[1] + kSprite / 2 <= kScreenW &&
              kCenterY - kSprite / 2 >= 0 &&
              kCenterY + kSprite / 2 <= kScreenH,
              "Both eyes must fit within the 240x320 portrait display");

struct Frame {
    int8_t gaze_x = 0;
    int8_t gaze_y = 0;
    uint8_t openness = 100;
};

inline bool operator==(const Frame& a, const Frame& b) {
    return a.gaze_x == b.gaze_x && a.gaze_y == b.gaze_y &&
           a.openness == b.openness;
}
inline bool operator!=(const Frame& a, const Frame& b) { return !(a == b); }

class Animator {
public:
    void Reset(uint32_t now, uint32_t seed) {
        rng_ = seed ? seed : 0xA90C7E31u;
        frame_ = Frame{};
        start_x_ = start_y_ = target_x_ = target_y_ = 0;
        motion_start_ = now;
        motion_duration_ = 350;
        next_gaze_ = now + 900 + NextRandom() % 800;
        next_blink_ = now + 1900 + NextRandom() % 2400;
        blink_start_ = now;
        blink_active_ = false;
        quick_blink_ = false;
    }

    Frame Tick(uint32_t now) {
        if (Reached(now, next_gaze_)) {
            start_x_ = frame_.gaze_x;
            start_y_ = frame_.gaze_y;
            static constexpr int8_t kX[] = {0, -4, -3, 2, 4, 0, 3, -2, 0};
            static constexpr int8_t kY[] = {0, -2, 0, 1, 2, 0, -1};
            target_x_ = kX[NextRandom() % (sizeof(kX) / sizeof(kX[0]))];
            target_y_ = kY[NextRandom() % (sizeof(kY) / sizeof(kY[0]))];
            motion_start_ = now;
            motion_duration_ = 280 + NextRandom() % 230;
            next_gaze_ = now + 1350 + NextRandom() % 2200;
        }
        const uint32_t elapsed = now - motion_start_;
        const uint32_t t = elapsed >= motion_duration_ ? 256 :
            elapsed * 256 / motion_duration_;
        // Smoothstep in integer Q8; exact endpoints and no floating point.
        const uint32_t ease = t * t * (768 - 2 * t) / 65536;
        frame_.gaze_x = Interpolate(start_x_, target_x_, ease);
        frame_.gaze_y = Interpolate(start_y_, target_y_, ease);

        if (!blink_active_ && Reached(now, next_blink_)) {
            blink_active_ = true;
            blink_start_ = now;
        }
        frame_.openness = 100;
        if (blink_active_) {
            const uint32_t blink_elapsed = now - blink_start_;
            if (blink_elapsed < 65) {
                frame_.openness = static_cast<uint8_t>(100 - blink_elapsed * 98 / 65);
            } else if (blink_elapsed < 105) {
                frame_.openness = 2;
            } else if (blink_elapsed < 205) {
                frame_.openness = static_cast<uint8_t>(2 + (blink_elapsed - 105) * 98 / 100);
            } else {
                blink_active_ = false;
                frame_.openness = 100;
                // Sometimes add a brief follow-up blink, otherwise a longer pause.
                if (quick_blink_) {
                    quick_blink_ = false;
                    next_blink_ = now + 2400 + NextRandom() % 3000;
                } else {
                    quick_blink_ = (NextRandom() % 9 == 0);
                    next_blink_ = now + (quick_blink_ ? 170 : 2200 + NextRandom() % 3600);
                }
            }
        }
        return frame_;
    }

private:
    static bool Reached(uint32_t now, uint32_t when) {
        return static_cast<int32_t>(now - when) >= 0;
    }
    static int8_t Interpolate(int8_t a, int8_t b, uint32_t ease) {
        const int32_t diff = static_cast<int32_t>(b) - a;
        const int32_t scaled = diff * static_cast<int32_t>(ease);
        const int32_t rounded = scaled >= 0 ? (scaled + 128) / 256 :
            -((-scaled + 128) / 256);
        return static_cast<int8_t>(static_cast<int32_t>(a) + rounded);
    }
    uint32_t NextRandom() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }
    uint32_t rng_ = 0xA90C7E31u;
    uint32_t next_gaze_ = 0, next_blink_ = 0;
    uint32_t motion_start_ = 0, blink_start_ = 0, motion_duration_ = 350;
    int8_t start_x_ = 0, start_y_ = 0, target_x_ = 0, target_y_ = 0;
    Frame frame_{};
    bool blink_active_ = false, quick_blink_ = false;
};

inline uint16_t Color565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline uint16_t Blend(uint16_t bg, uint16_t fg, unsigned alpha) {
    if (alpha >= 255) return fg;
    const unsigned inv = 255 - alpha;
    const unsigned r = (((bg >> 11) & 31) * inv + ((fg >> 11) & 31) * alpha + 127) / 255;
    const unsigned g = (((bg >> 5) & 63) * inv + ((fg >> 5) & 63) * alpha + 127) / 255;
    const unsigned b = ((bg & 31) * inv + (fg & 31) * alpha + 127) / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// The frame image has already had the two reference eyes removed. Rebuilding
// each sprite from that immutable flash background avoids trails and flicker.
inline void DrawEye(uint16_t* dst, const uint16_t* background,
                    int eye_index, const Frame& frame) {
    if (!dst || !background || eye_index < 0 || eye_index > 1) return;
    const int origin_x = kCentersX[eye_index] - kSprite / 2;
    const int origin_y = kCenterY - kSprite / 2;
    constexpr uint16_t kSoftGlow = 0x5E9D; // pastel turquoise on navy face
    constexpr uint16_t kOuterRing = 0x6EFC;
    constexpr uint16_t kEyeWhite = 0xEFFF; // gentle ice-white, not neon
    constexpr uint16_t kIrisBlue = 0x3C9E;
    constexpr uint16_t kDeepBlue = 0x0A2F;
    constexpr uint16_t kWhite = 0xFFFF;
    const int half_open = (19 * frame.openness) / 100;
    for (int y = 0; y < kSprite; ++y) {
        for (int x = 0; x < kSprite; ++x) {
            const int px = origin_x + x;
            const int py = origin_y + y;
            const int dx = x - kSprite / 2;
            const int dy = y - kSprite / 2;
            const int r2 = dx * dx + dy * dy;
            uint16_t pixel = background[py * kScreenW + px];

            // Each frame starts with unmodified flash pixels: no old-frame
            // artifacts when the pupil glances, eyelid moves or status exits.
            if (frame.openness <= 4) {
                // Small upward-curved, smiling eyelid on the dark faceplate.
                const int curve = dx * dx / 125;
                if (dx >= -16 && dx <= 16 && dy >= curve - 1 && dy <= curve + 1)
                    pixel = kOuterRing;
            } else {
                // A subtle halo, feathered at its edge, for warm friendly eyes.
                if (r2 <= 25 * 25 && r2 > 20 * 20 &&
                    dy >= -half_open - 2 && dy <= half_open + 2) {
                    const unsigned alpha = static_cast<unsigned>(
                        (25 * 25 - r2) * 100 / (25 * 25 - 20 * 20));
                    pixel = Blend(pixel, kSoftGlow, alpha);
                }
                if (r2 <= 20 * 20 && dy >= -half_open && dy <= half_open) {
                    if (r2 > 17 * 17) {
                        pixel = kOuterRing;
                    } else {
                        pixel = kEyeWhite;
                        // The irises and the reflections move as one object,
                        // unlike a white dot fixed in an otherwise still ring.
                        const int u = dx - frame.gaze_x;
                        const int v = dy - frame.gaze_y;
                        const int iris_r2 = u * u + v * v;
                        if (iris_r2 <= 12 * 12) pixel = kIrisBlue;
                        if (iris_r2 <= 7 * 7) pixel = kDeepBlue;
                        if (iris_r2 <= 12 * 12 &&
                            (u + 4) * (u + 4) + (v + 4) * (v + 4) <= 3 * 3)
                            pixel = kWhite;
                        if (iris_r2 <= 12 * 12 &&
                            (u - 5) * (u - 5) + (v - 4) * (v - 4) <= 1)
                            pixel = kWhite;
                    }
                }
            }
            dst[y * kSprite + x] = pixel;
        }
    }
}

}  // namespace smartmedivend_eyes
