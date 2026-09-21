import os
import shlex
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main" / "boards" / "smartmedivend-s3"
AUDIO_DIR = ROOT / "main" / "audio"


class SmartMediVendAudioTest(unittest.TestCase):
    def compile_and_run(self, source_text):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            source = build_dir / "smartmedivend_audio_test.cc"
            source.write_text(textwrap.dedent(source_text), encoding="utf-8")
            executable = build_dir / "smartmedivend_audio_test.exe"
            compiler = shlex.split(os.environ.get("CXX", "c++"))
            command = compiler + [
                "-std=c++20",
                f"-I{BOARD_DIR}",
                f"-I{AUDIO_DIR}",
                str(source),
                "-o",
                str(executable),
            ]
            environment = os.environ.copy()
            compiler_dir = str(Path(compiler[0]).resolve().parent)
            environment["PATH"] = compiler_dir + os.pathsep + environment.get("PATH", "")
            compiled = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=30,
                cwd=build_dir,
                env=environment,
            )
            self.assertEqual(
                compiled.returncode,
                0,
                msg=f"C++ test compilation failed:\n{compiled.stdout}{compiled.stderr}",
            )
            executed = subprocess.run(
                [executable],
                capture_output=True,
                text=True,
                timeout=30,
                cwd=build_dir,
                env=environment,
            )
            self.assertEqual(
                executed.returncode,
                0,
                msg=f"C++ test failed:\n{executed.stdout}{executed.stderr}",
            )

    def test_inmp441_conversion_preserves_headroom_without_adaptive_gain(self):
        # Catches regressions to the old >>12 + pre-clamp conversion and its
        # frame-driven gain ramp, both of which distort speech transients.
        self.compile_and_run(
            r"""
            #include <cassert>
            #include <cstdint>
            #include <cstdlib>

            #include "smartmedivend_mic_processor.h"

            int main() {
                smv::MicProcessor processor;

                const int32_t loud_raw[] = {163840000, -163840000};
                int16_t loud_pcm[2] = {};
                const auto loud = processor.Process(loud_raw, loud_pcm, 2);
                assert(loud.clipped_input == 0);
                assert(loud.clipped_output == 0);
                assert(loud.gain_q8 == 256);
                assert(std::abs(static_cast<int>(loud_pcm[0])) >= 9500);
                assert(std::abs(static_cast<int>(loud_pcm[0])) <= 10000);
                assert(std::abs(static_cast<int>(loud_pcm[1])) >= 9500);
                assert(std::abs(static_cast<int>(loud_pcm[1])) <= 10000);

                processor.Reset();
                const int32_t quiet_raw[] = {8192000, -8192000};
                int16_t quiet_pcm[2] = {};
                const auto quiet = processor.Process(quiet_raw, quiet_pcm, 2);
                assert(quiet.gain_q8 == 256);
                assert(std::abs(static_cast<int>(quiet_pcm[0])) <= 500);
                assert(std::abs(static_cast<int>(quiet_pcm[1])) <= 500);
            }
            """
        )

    def test_local_endpoint_stops_once_only_after_speech(self):
        # Catches false end-of-utterance at startup, duplicate stop messages,
        # and failure to re-arm the controller for the next listening turn.
        self.compile_and_run(
            r"""
            #include <cassert>

            #include "vad_endpoint.h"

            int main() {
                VadEndpoint endpoint;
                assert(!endpoint.OnVadState(false));
                assert(!endpoint.OnVadState(true));
                assert(endpoint.OnVadState(false));
                assert(!endpoint.OnVadState(false));
                assert(!endpoint.OnVadState(true));

                endpoint.Reset();
                assert(!endpoint.OnVadState(true));
                assert(endpoint.OnVadState(false));
            }
            """
        )

    def test_frontend_policy_enables_clean_capture_and_700_ms_endpoint(self):
        # Catches accidental loss of the board-selected NS/AGC settings or a
        # return to the former 100-ms VAD silence window.
        self.compile_and_run(
            r"""
            #include <cassert>

            #define CONFIG_USE_AUDIO_NOISE_SUPPRESSION 1
            #define CONFIG_USE_AUDIO_AGC 1
            #define CONFIG_AUDIO_AGC_COMPRESSION_GAIN_DB 9
            #define CONFIG_AUDIO_AGC_TARGET_LEVEL_DBFS 9
            #define CONFIG_USE_LOCAL_VAD_ENDPOINT 1
            #define CONFIG_LOCAL_VAD_SILENCE_MS 700
            #include "audio_frontend_policy.h"

            int main() {
                constexpr auto policy = GetAudioFrontendPolicy();
                static_assert(policy.noise_suppression_enabled);
                static_assert(policy.automatic_gain_control_enabled);
                static_assert(policy.agc_compression_gain_db == 9);
                static_assert(policy.agc_target_level_dbfs == 9);
                static_assert(policy.vad_silence_ms == 700);
                static_assert(!NeedsAudioInputWarmup(true));
                static_assert(NeedsAudioInputWarmup(false));
            }
            """
        )


if __name__ == "__main__":
    unittest.main()
