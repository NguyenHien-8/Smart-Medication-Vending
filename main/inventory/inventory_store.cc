#include "inventory_store.h"

#include <algorithm>
#include <array>
#include <limits>

namespace smv {
namespace {
constexpr std::string_view kSnapshotA = "snapshot_a";
constexpr std::string_view kSnapshotB = "snapshot_b";
constexpr uint32_t kMaximumCount = 10000;
constexpr size_t kEncodedSize = 98;

void AppendU8(std::vector<uint8_t>& bytes, uint8_t value) { bytes.push_back(value); }

void AppendU16(std::vector<uint8_t>& bytes, uint16_t value) {
    for (size_t index = 0; index < 2; ++index)
        bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (size_t index = 0; index < 4; ++index)
        bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

void AppendU64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (size_t index = 0; index < 8; ++index)
        bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

bool ReadU8(std::span<const uint8_t> bytes, size_t& offset, uint8_t& value) {
    if (offset >= bytes.size())
        return false;
    value = bytes[offset++];
    return true;
}

bool ReadU16(std::span<const uint8_t> bytes, size_t& offset, uint16_t& value) {
    if (offset + 2 > bytes.size())
        return false;
    value = 0;
    for (size_t index = 0; index < 2; ++index)
        value |= static_cast<uint16_t>(bytes[offset++]) << (index * 8);
    return true;
}

bool ReadU32(std::span<const uint8_t> bytes, size_t& offset, uint32_t& value) {
    if (offset + 4 > bytes.size())
        return false;
    value = 0;
    for (size_t index = 0; index < 4; ++index)
        value |= static_cast<uint32_t>(bytes[offset++]) << (index * 8);
    return true;
}

bool ReadU64(std::span<const uint8_t> bytes, size_t& offset, uint64_t& value) {
    if (offset + 8 > bytes.size())
        return false;
    value = 0;
    for (size_t index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(bytes[offset++]) << (index * 8);
    return true;
}

uint32_t Crc32(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffu;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
}  // namespace

std::vector<uint8_t> InventoryStore::Encode(const State& state) {
    std::vector<uint8_t> bytes;
    bytes.reserve(kEncodedSize);
    bytes.insert(bytes.end(), {'S', 'M', 'V', '1'});
    AppendU8(bytes, 1);
    AppendU32(bytes, state.revision);
    for (const uint32_t count : state.counts)
        AppendU32(bytes, count);
    AppendU16(bytes, state.valid_mask);
    AppendU8(bytes, state.pending ? 1 : 0);
    AppendU8(bytes, state.pending_channel);
    AppendU64(bytes, state.pending_transaction_id);
    AppendU64(bytes, state.last_transaction_id);
    AppendU8(bytes, static_cast<uint8_t>(state.last_outcome));
    AppendU32(bytes, Crc32(bytes));
    return bytes;
}

InventoryStore::DecodeResult InventoryStore::Decode(std::span<const uint8_t> bytes, State& state) {
    if (bytes.size() != kEncodedSize || !std::equal(bytes.begin(), bytes.begin() + 4, "SMV1"))
        return DecodeResult::kCorrupt;
    size_t crc_offset = bytes.size() - 4;
    size_t crc_read_offset = crc_offset;
    uint32_t stored_crc = 0;
    if (!ReadU32(bytes, crc_read_offset, stored_crc) ||
        Crc32(bytes.first(crc_offset)) != stored_crc)
        return DecodeResult::kCorrupt;

    size_t offset = 4;
    uint8_t schema = 0;
    uint8_t pending = 0;
    uint8_t outcome = 0;
    State decoded;
    if (!ReadU8(bytes, offset, schema) || schema != 1 || !ReadU32(bytes, offset, decoded.revision))
        return DecodeResult::kCorrupt;
    if (decoded.revision == 0)
        return DecodeResult::kCorrupt;
    if (decoded.revision == std::numeric_limits<uint32_t>::max())
        return DecodeResult::kRevisionExhausted;
    for (uint32_t& count : decoded.counts) {
        if (!ReadU32(bytes, offset, count) || count > kMaximumCount)
            return DecodeResult::kCorrupt;
    }
    if (!ReadU16(bytes, offset, decoded.valid_mask) || decoded.valid_mask == 0 ||
        !ReadU8(bytes, offset, pending) || pending > 1 ||
        !ReadU8(bytes, offset, decoded.pending_channel) ||
        !ReadU64(bytes, offset, decoded.pending_transaction_id) ||
        !ReadU64(bytes, offset, decoded.last_transaction_id) || !ReadU8(bytes, offset, outcome) ||
        offset != crc_offset || outcome > static_cast<uint8_t>(LastOutcome::kCancelled)) {
        return DecodeResult::kCorrupt;
    }

    decoded.pending = pending == 1;
    decoded.last_outcome = static_cast<LastOutcome>(outcome);
    if (decoded.pending) {
        if (decoded.pending_channel >= kVendingChannelCount ||
            (decoded.valid_mask & (1u << decoded.pending_channel)) == 0 ||
            decoded.pending_transaction_id == 0) {
            return DecodeResult::kCorrupt;
        }
    } else if (decoded.pending_channel != kInvalidVendingChannel ||
               decoded.pending_transaction_id != 0) {
        return DecodeResult::kCorrupt;
    }
    if ((decoded.last_outcome == LastOutcome::kNone) != (decoded.last_transaction_id == 0))
        return DecodeResult::kCorrupt;
    state = decoded;
    return DecodeResult::kValid;
}

InventoryResult InventoryStore::Fail(InventoryResult result) {
    snapshot_ = {};
    last_result_ = result;
    return result;
}

void InventoryStore::Publish() {
    snapshot_.available = true;
    snapshot_.revision = state_.revision;
    snapshot_.counts = state_.counts;
    snapshot_.valid_mask = state_.valid_mask;
    snapshot_.transaction_pending = state_.pending;
}

InventoryResult InventoryStore::Load() {
    std::vector<uint8_t> bytes_a;
    std::vector<uint8_t> bytes_b;
    const bool present_a = backend_.ReadBlob(kSnapshotA, bytes_a);
    const bool present_b = backend_.ReadBlob(kSnapshotB, bytes_b);
    if (!present_a && !present_b)
        return Fail(InventoryResult::kUnavailable);
    if (!present_a || !present_b)
        return Fail(InventoryResult::kCorrupt);

    State state_a;
    State state_b;
    const DecodeResult result_a = present_a ? Decode(bytes_a, state_a) : DecodeResult::kCorrupt;
    const DecodeResult result_b = present_b ? Decode(bytes_b, state_b) : DecodeResult::kCorrupt;
    if (result_a == DecodeResult::kRevisionExhausted ||
        result_b == DecodeResult::kRevisionExhausted) {
        return Fail(InventoryResult::kRevisionExhausted);
    }

    const bool valid_a = result_a == DecodeResult::kValid;
    const bool valid_b = result_b == DecodeResult::kValid;
    if (!valid_a || !valid_b)
        return Fail(InventoryResult::kCorrupt);
    if (state_a.revision == state_b.revision && bytes_a != bytes_b)
        return Fail(InventoryResult::kAmbiguousRevision);

    if (state_a.revision >= state_b.revision) {
        state_ = state_a;
        active_slot_ = 'a';
    } else {
        state_ = state_b;
        active_slot_ = 'b';
    }
    Publish();
    last_result_ = state_.pending ? InventoryResult::kPendingTransaction : InventoryResult::kOk;
    return last_result_;
}

bool InventoryStore::WriteVerified(std::string_view key, const State& next, State& decoded) {
    const std::vector<uint8_t> encoded = Encode(next);
    if (!backend_.WriteBlob(key, encoded))
        return false;
    std::vector<uint8_t> verified;
    return backend_.ReadBlob(key, verified) && verified == encoded &&
           Decode(verified, decoded) == DecodeResult::kValid;
}

InventoryResult InventoryStore::Commit(const State& next) {
    const std::string_view key = active_slot_ == 'a' ? kSnapshotB : kSnapshotA;
    const char slot = active_slot_ == 'a' ? 'b' : 'a';
    State decoded;
    if (!WriteVerified(key, next, decoded)) {
        return Fail(InventoryResult::kWriteFailed);
    }
    state_ = decoded;
    active_slot_ = slot;
    Publish();
    last_result_ = InventoryResult::kOk;
    return last_result_;
}

InventoryResult InventoryStore::Provision(const std::array<uint32_t, kVendingChannelCount>& counts,
                                          uint16_t valid_mask) {
    if (valid_mask == 0 || std::any_of(counts.begin(), counts.end(),
                                       [](uint32_t value) { return value > kMaximumCount; })) {
        return Fail(InventoryResult::kCorrupt);
    }
    if (snapshot_.available && state_.pending)
        return (last_result_ = InventoryResult::kPendingTransaction);
    if (snapshot_.available && state_.revision == std::numeric_limits<uint32_t>::max() - 1)
        return Fail(InventoryResult::kRevisionExhausted);

    State next;
    next.revision = snapshot_.available ? state_.revision + 1 : 1;
    next.counts = counts;
    next.valid_mask = valid_mask;
    if (!snapshot_.available) {
        State decoded_a;
        State decoded_b;
        if (!WriteVerified(kSnapshotA, next, decoded_a) ||
            !WriteVerified(kSnapshotB, next, decoded_b)) {
            return Fail(InventoryResult::kWriteFailed);
        }
        state_ = decoded_b;
        active_slot_ = 'b';
        Publish();
        last_result_ = InventoryResult::kOk;
        return last_result_;
    }
    return Commit(next);
}

std::optional<ReservationToken> InventoryStore::Reserve(uint64_t transaction_id, uint8_t channel,
                                                        uint32_t expected_revision) {
    if (!snapshot_.available) {
        last_result_ = InventoryResult::kUnavailable;
        return std::nullopt;
    }
    if (state_.pending) {
        last_result_ = InventoryResult::kPendingTransaction;
        return std::nullopt;
    }
    if (expected_revision != state_.revision) {
        last_result_ = InventoryResult::kStaleRevision;
        return std::nullopt;
    }
    if (transaction_id == 0 || transaction_id == state_.last_transaction_id) {
        last_result_ = InventoryResult::kDuplicateTransaction;
        return std::nullopt;
    }
    if (channel >= kVendingChannelCount || (state_.valid_mask & (1u << channel)) == 0 ||
        state_.counts[channel] == 0) {
        last_result_ = InventoryResult::kOutOfStock;
        return std::nullopt;
    }
    if (state_.revision == std::numeric_limits<uint32_t>::max() - 1) {
        Fail(InventoryResult::kRevisionExhausted);
        return std::nullopt;
    }

    State next = state_;
    ++next.revision;
    --next.counts[channel];
    next.pending = true;
    next.pending_channel = channel;
    next.pending_transaction_id = transaction_id;
    if (Commit(next) != InventoryResult::kOk)
        return std::nullopt;
    return ReservationToken{transaction_id, next.revision, channel};
}

InventoryResult InventoryStore::Complete(const ReservationToken& token) {
    if (!snapshot_.available)
        return (last_result_ = InventoryResult::kUnavailable);
    if (!state_.pending || token.transaction_id != state_.pending_transaction_id ||
        token.channel != state_.pending_channel || token.reserved_revision != state_.revision)
        return (last_result_ = InventoryResult::kStaleRevision);
    if (state_.revision == std::numeric_limits<uint32_t>::max() - 1)
        return Fail(InventoryResult::kRevisionExhausted);

    State next = state_;
    ++next.revision;
    next.pending = false;
    next.pending_channel = kInvalidVendingChannel;
    next.pending_transaction_id = 0;
    next.last_transaction_id = token.transaction_id;
    next.last_outcome = LastOutcome::kCommandSentUnverified;
    return Commit(next);
}

InventoryResult InventoryStore::CancelBeforePulse(const ReservationToken& token) {
    if (!snapshot_.available)
        return (last_result_ = InventoryResult::kUnavailable);
    if (!state_.pending || token.transaction_id != state_.pending_transaction_id ||
        token.channel != state_.pending_channel || token.reserved_revision != state_.revision)
        return (last_result_ = InventoryResult::kStaleRevision);
    if (state_.revision == std::numeric_limits<uint32_t>::max() - 1 ||
        state_.counts[token.channel] == kMaximumCount)
        return Fail(InventoryResult::kRevisionExhausted);

    State next = state_;
    ++next.revision;
    ++next.counts[token.channel];
    next.pending = false;
    next.pending_channel = kInvalidVendingChannel;
    next.pending_transaction_id = 0;
    next.last_transaction_id = token.transaction_id;
    next.last_outcome = LastOutcome::kCancelled;
    return Commit(next);
}
}  // namespace smv
