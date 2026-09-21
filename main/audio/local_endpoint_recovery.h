#pragma once

#include <chrono>

// The local VAD may produce an endpoint without a usable utterance (for example
// after a late playback/VAD event). Auto-mode must not leave the device in
// "listening" with its microphone stopped until the server's idle timeout.
// This controller only retries an *unacknowledged* local stop, once per turn;
// the next attempt leaves endpointing to Xiaozhi's existing auto mode.
class LocalEndpointRecovery {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto kResponseTimeout = std::chrono::seconds(5);

    void Reset() {
        awaiting_response_ = false;
        server_endpoint_only_ = false;
    }

    bool CanSendLocalStop() const { return !awaiting_response_ && !server_endpoint_only_; }

    void OnLocalStopSent(TimePoint now) {
        awaiting_response_ = true;
        stop_time_ = now;
    }

    void OnServerResponse() { awaiting_response_ = false; }

    bool RecoverIfStalled(TimePoint now) {
        if (!awaiting_response_ || now - stop_time_ < kResponseTimeout) {
            return false;
        }
        awaiting_response_ = false;
        server_endpoint_only_ = true;
        return true;
    }

    bool AwaitingResponse() const { return awaiting_response_; }
    bool ServerEndpointOnly() const { return server_endpoint_only_; }

private:
    TimePoint stop_time_{};
    bool awaiting_response_ = false;
    bool server_endpoint_only_ = false;
};
