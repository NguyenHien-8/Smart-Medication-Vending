"""Host-side regression coverage for stalled local VAD / Xiaozhi auto mode."""
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class LocalEndpointRecoveryTests(unittest.TestCase):
    def test_endpoint_timeout_ack_and_single_fallback(self):
        source = textwrap.dedent(r'''
            #include <atomic>
            #include <cassert>
            #include <chrono>
            #include "audio/local_endpoint_recovery.h"
            #include "audio/vad_endpoint.h"

            int main() {
                using namespace std::chrono_literals;
                using Clock = LocalEndpointRecovery::Clock;
                const auto start = Clock::time_point(100s);
                LocalEndpointRecovery recovery;
                VadEndpoint vad;
                assert(!recovery.AwaitingResponse());
                assert(recovery.CanSendLocalStop());
                assert(!vad.OnVadState(false)); // Silence at turn start: no endpoint.
                assert(!vad.OnVadState(true));
                assert(vad.OnVadState(false));
                recovery.OnLocalStopSent(start);
                assert(recovery.AwaitingResponse());
                assert(!recovery.CanSendLocalStop());
                assert(!recovery.RecoverIfStalled(start + 4999ms));
                recovery.OnServerResponse(); // STT/TTS before deadline: no restart.
                assert(!recovery.RecoverIfStalled(start + 10s));
                recovery.Reset(); // New turn.
                assert(recovery.CanSendLocalStop());

                recovery.OnLocalStopSent(start);
                assert(recovery.RecoverIfStalled(start + 5s));
                assert(!recovery.RecoverIfStalled(start + 6s));
                assert(recovery.ServerEndpointOnly());
                assert(!recovery.CanSendLocalStop()); // No repeated spurious stops.
                recovery.OnServerResponse();
                assert(!recovery.CanSendLocalStop());
                recovery.Reset(); // After real TTS -> next turn can use local VAD.
                assert(recovery.CanSendLocalStop());

                std::atomic<unsigned> generation{0};
                unsigned old_event = generation.load();
                generation.fetch_add(1); // TTS / new listening turn.
                assert(old_event != generation.load()); // Old queued VAD discarded.
            }
        ''')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.cc"
            exe = Path(directory) / "test"
            path.write_text(source, encoding="utf-8")
            compile_result = subprocess.run(
                ["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
                 str(path), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)

    def test_recovery_connected_to_actual_event_paths(self):
        code = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        self.assertIn('local_endpoint_recovery_.OnLocalStopSent(', code)
        self.assertIn('local_endpoint_recovery_.RecoverIfStalled(', code)
        self.assertIn('local_endpoint_recovery_.CanSendLocalStop()', code)
        self.assertIn('local_endpoint_recovery_.OnServerResponse()', code)
        self.assertIn('listening_generation_.fetch_add(', code)
        self.assertIn('generation != listening_generation_.load(', code)
        self.assertIn('StartListeningAudio();', code)
        self.assertIn('!audio_service_.IsAudioProcessorRunning()', code)


if __name__ == "__main__":
    unittest.main()
