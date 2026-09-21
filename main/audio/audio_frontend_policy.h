#pragma once

struct AudioFrontendPolicy {
    bool noise_suppression_enabled;
    bool automatic_gain_control_enabled;
    int agc_compression_gain_db;
    int agc_target_level_dbfs;
    int vad_silence_ms;
};

constexpr AudioFrontendPolicy GetAudioFrontendPolicy() {
    AudioFrontendPolicy policy{
#if defined(CONFIG_USE_AUDIO_NOISE_SUPPRESSION) && CONFIG_USE_AUDIO_NOISE_SUPPRESSION
        .noise_suppression_enabled = true,
#else
        .noise_suppression_enabled = false,
#endif
#if defined(CONFIG_USE_AUDIO_AGC) && CONFIG_USE_AUDIO_AGC
        .automatic_gain_control_enabled = true,
        .agc_compression_gain_db = CONFIG_AUDIO_AGC_COMPRESSION_GAIN_DB,
        .agc_target_level_dbfs = CONFIG_AUDIO_AGC_TARGET_LEVEL_DBFS,
#else
        .automatic_gain_control_enabled = false,
        .agc_compression_gain_db = 0,
        .agc_target_level_dbfs = 0,
#endif
#if defined(CONFIG_USE_LOCAL_VAD_ENDPOINT) && CONFIG_USE_LOCAL_VAD_ENDPOINT
        .vad_silence_ms = CONFIG_LOCAL_VAD_SILENCE_MS,
#else
        .vad_silence_ms = 100,
#endif
    };
    return policy;
}

// An already-running I2S input is warm. Repeating the 120-ms startup delay on
// every conversational turn clips the first syllable without settling hardware.
constexpr bool NeedsAudioInputWarmup(bool input_enabled) { return !input_enabled; }
