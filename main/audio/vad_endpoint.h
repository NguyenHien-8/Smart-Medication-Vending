#pragma once

// Tracks one listening turn. A VAD silence transition may end the utterance
// only after speech has started, and it may do so only once until Reset().
class VadEndpoint {
public:
    void Reset() {
        speech_started_ = false;
        stop_sent_ = false;
    }

    bool OnVadState(bool speaking) {
        if (speaking) {
            speech_started_ = true;
            return false;
        }
        if (!speech_started_ || stop_sent_) {
            return false;
        }
        stop_sent_ = true;
        return true;
    }

private:
    bool speech_started_ = false;
    bool stop_sent_ = false;
};
