#pragma once

#include "vending/vending_types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace smv {
class InventoryBackend {
public:
    virtual ~InventoryBackend() = default;
    virtual bool ReadBlob(std::string_view key, std::vector<uint8_t>& value) = 0;
    virtual bool WriteBlob(std::string_view key, std::span<const uint8_t> value) = 0;
};

enum class InventoryResult {
    kOk,
    kUnavailable,
    kCorrupt,
    kAmbiguousRevision,
    kPendingTransaction,
    kStaleRevision,
    kOutOfStock,
    kDuplicateTransaction,
    kRevisionExhausted,
    kWriteFailed,
};

class InventoryStore {
public:
    explicit InventoryStore(InventoryBackend& backend) : backend_(backend) {}

    InventoryResult Load();
    InventoryResult Provision(
        const std::array<uint32_t, kVendingChannelCount>& counts,
        uint16_t valid_mask = static_cast<uint16_t>((1u << kVendingChannelCount) - 1u));
    std::optional<ReservationToken> Reserve(uint64_t transaction_id, uint8_t channel,
                                            uint32_t expected_revision);
    InventoryResult Complete(const ReservationToken& token);
    InventoryResult CancelBeforePulse(const ReservationToken& token);

    const StockSnapshot& Snapshot() const { return snapshot_; }
    InventoryResult last_result() const { return last_result_; }

private:
    enum class LastOutcome : uint8_t { kNone = 0, kCommandSentUnverified = 1, kCancelled = 2 };

    struct State {
        uint32_t revision = 0;
        std::array<uint32_t, kVendingChannelCount> counts{};
        uint16_t valid_mask = 0;
        bool pending = false;
        uint8_t pending_channel = kInvalidVendingChannel;
        uint64_t pending_transaction_id = 0;
        uint64_t last_transaction_id = 0;
        LastOutcome last_outcome = LastOutcome::kNone;
    };

    enum class DecodeResult { kValid, kCorrupt, kRevisionExhausted };

    static std::vector<uint8_t> Encode(const State& state);
    static DecodeResult Decode(std::span<const uint8_t> bytes, State& state);
    InventoryResult Commit(const State& next);
    InventoryResult Fail(InventoryResult result);
    void Publish();

    InventoryBackend& backend_;
    State state_;
    StockSnapshot snapshot_;
    InventoryResult last_result_ = InventoryResult::kUnavailable;
    char active_slot_ = '\0';
};
}  // namespace smv
