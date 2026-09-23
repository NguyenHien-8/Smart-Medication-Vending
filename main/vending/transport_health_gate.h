#pragma once

#include <atomic>
#include <cstdint>

namespace smv {
// A disconnected transport must invalidate medication confirmation even when
// the physical Wi-Fi interface never emits a NetworkEvent. Generation tags
// prevent a queued/stale reconnect from reopening the gate after a newer loss.
class TransportHealthGate final {
public:
    bool IsOperational() const { return (state_.load(std::memory_order_acquire) & 1u) == 0; }

    uint32_t Snapshot() const { return state_.load(std::memory_order_acquire); }

    void MarkLost() {
        uint32_t previous = state_.load(std::memory_order_acquire);
        // The low bit is the fault state; high bits are the loss epoch.
        while (!state_.compare_exchange_weak(
            previous, (previous + 2u) | 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }

    bool RestoreIfUnchanged(uint32_t observed) {
        return state_.compare_exchange_strong(observed, observed & ~1u, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

private:
    // Deny medication actions at boot until a transport handshake completes.
    std::atomic<uint32_t> state_{1};
};
}  // namespace smv
