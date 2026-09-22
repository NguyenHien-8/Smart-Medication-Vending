#include "inventory/inventory_store.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void Check(bool passed, const char* message) {
    if (!passed)
        throw std::runtime_error(message);
}

class MemoryInventoryBackend : public smv::InventoryBackend {
public:
    bool ReadBlob(std::string_view key, std::vector<uint8_t>& value) override {
        const auto found = blobs.find(std::string(key));
        if (found == blobs.end())
            return false;
        value = found->second;
        return true;
    }

    bool WriteBlob(std::string_view key, std::span<const uint8_t> value) override {
        if (fail_write)
            return false;
        blobs[std::string(key)] = std::vector<uint8_t>(value.begin(), value.end());
        if (corrupt_after_write && !blobs[std::string(key)].empty())
            blobs[std::string(key)].back() ^= 0x5a;
        return true;
    }

    std::map<std::string, std::vector<uint8_t>> blobs;
    bool fail_write = false;
    bool corrupt_after_write = false;
};

uint32_t Crc32(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffu;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void SetLe32(std::vector<uint8_t>& blob, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index)
        blob[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

void RepairCrc(std::vector<uint8_t>& blob) {
    SetLe32(blob, blob.size() - 4, Crc32(std::span(blob).first(blob.size() - 4)));
}

std::array<uint32_t, smv::kVendingChannelCount> Counts(uint32_t channel_zero) {
    std::array<uint32_t, smv::kVendingChannelCount> counts{};
    counts[0] = channel_zero;
    return counts;
}
}  // namespace

int main() {
    try {
        int count = 0;
        MemoryInventoryBackend empty_backend;
        smv::InventoryStore empty(empty_backend);
        Check(empty.Load() == smv::InventoryResult::kUnavailable, "empty NVS accepted");
        ++count;

        MemoryInventoryBackend provision_backend;
        smv::InventoryStore provisioned(provision_backend);
        Check(provisioned.Provision(Counts(2)) == smv::InventoryResult::kOk &&
                  provisioned.Snapshot().available && provisioned.Snapshot().revision == 1 &&
                  provisioned.Snapshot().counts[0] == 2,
              "valid provisioning failed");
        ++count;
        smv::InventoryStore first_provision_reboot(provision_backend);
        Check(first_provision_reboot.Load() == smv::InventoryResult::kOk &&
                  first_provision_reboot.Snapshot().counts[0] == 2,
              "first technician provisioning did not survive reboot");
        ++count;
        Check(provisioned.Provision(Counts(3)) == smv::InventoryResult::kOk,
              "second provisioning failed");
        smv::InventoryStore newest(provision_backend);
        Check(newest.Load() == smv::InventoryResult::kOk && newest.Snapshot().revision == 2 &&
                  newest.Snapshot().counts[0] == 3,
              "newest valid slot was not selected");
        ++count;

        MemoryInventoryBackend fallback_backend = provision_backend;
        fallback_backend.blobs["snapshot_b"].back() ^= 1;
        smv::InventoryStore fallback(fallback_backend);
        Check(fallback.Load() == smv::InventoryResult::kCorrupt && !fallback.Snapshot().available,
              "one corrupt slot rolled inventory back instead of locking");
        ++count;

        MemoryInventoryBackend ambiguous_backend = provision_backend;
        ambiguous_backend.blobs["snapshot_a"] = ambiguous_backend.blobs["snapshot_b"];
        SetLe32(ambiguous_backend.blobs["snapshot_a"], 9, 4);
        RepairCrc(ambiguous_backend.blobs["snapshot_a"]);
        smv::InventoryStore ambiguous(ambiguous_backend);
        Check(ambiguous.Load() == smv::InventoryResult::kAmbiguousRevision,
              "equal divergent revisions were accepted");
        ++count;

        MemoryInventoryBackend overflow_backend;
        smv::InventoryStore overflow_seed(overflow_backend);
        Check(overflow_seed.Provision(Counts(1)) == smv::InventoryResult::kOk,
              "overflow fixture provisioning failed");
        SetLe32(overflow_backend.blobs["snapshot_a"], 9, 10001);
        RepairCrc(overflow_backend.blobs["snapshot_a"]);
        smv::InventoryStore overflow(overflow_backend);
        Check(overflow.Load() == smv::InventoryResult::kCorrupt, "out-of-range count was accepted");
        ++count;

        MemoryInventoryBackend exhausted_backend;
        smv::InventoryStore exhausted_seed(exhausted_backend);
        Check(exhausted_seed.Provision(Counts(1)) == smv::InventoryResult::kOk,
              "exhaustion fixture provisioning failed");
        SetLe32(exhausted_backend.blobs["snapshot_a"], 5, UINT32_MAX);
        RepairCrc(exhausted_backend.blobs["snapshot_a"]);
        smv::InventoryStore exhausted(exhausted_backend);
        Check(exhausted.Load() == smv::InventoryResult::kRevisionExhausted,
              "exhausted revision was accepted");
        ++count;

        MemoryInventoryBackend corrupt_write_backend;
        corrupt_write_backend.corrupt_after_write = true;
        smv::InventoryStore corrupt_write(corrupt_write_backend);
        Check(corrupt_write.Provision(Counts(1)) == smv::InventoryResult::kWriteFailed &&
                  !corrupt_write.Snapshot().available,
              "corrupt write/read-back did not lock inventory");
        ++count;

        MemoryInventoryBackend reserve_backend;
        smv::InventoryStore reserve(reserve_backend);
        Check(reserve.Provision(Counts(2)) == smv::InventoryResult::kOk,
              "reserve fixture provisioning failed");
        const uint32_t initial_revision = reserve.Snapshot().revision;
        Check(!reserve.Reserve(2, 0, initial_revision + 1).has_value() &&
                  reserve.last_result() == smv::InventoryResult::kStaleRevision,
              "stale expected revision was accepted");
        ++count;
        const auto token = reserve.Reserve(0x1122334455667788ULL, 0, initial_revision);
        Check(token.has_value() && reserve.Snapshot().counts[0] == 1 &&
                  reserve.Snapshot().transaction_pending &&
                  token->reserved_revision == initial_revision + 1,
              "unit was not durably reserved");
        ++count;
        Check(!reserve.Reserve(3, 0, initial_revision).has_value() &&
                  reserve.last_result() == smv::InventoryResult::kPendingTransaction,
              "stale or pending reservation was accepted");
        ++count;

        smv::InventoryStore pending_reboot(reserve_backend);
        Check(pending_reboot.Load() == smv::InventoryResult::kPendingTransaction &&
                  pending_reboot.Snapshot().transaction_pending,
              "unfinished transaction did not lock reboot");
        ++count;

        Check(reserve.Complete(*token) == smv::InventoryResult::kOk &&
                  reserve.Snapshot().counts[0] == 1 && !reserve.Snapshot().transaction_pending,
              "completion decremented twice or left pending state");
        ++count;
        Check(!reserve.Reserve(token->transaction_id, 0, reserve.Snapshot().revision).has_value() &&
                  reserve.last_result() == smv::InventoryResult::kDuplicateTransaction,
              "duplicate transaction ID was accepted");
        ++count;

        MemoryInventoryBackend cancel_backend;
        smv::InventoryStore cancel(cancel_backend);
        Check(cancel.Provision(Counts(1)) == smv::InventoryResult::kOk,
              "cancel fixture provisioning failed");
        const auto cancel_token = cancel.Reserve(9, 0, cancel.Snapshot().revision);
        Check(cancel_token.has_value() &&
                  cancel.CancelBeforePulse(*cancel_token) == smv::InventoryResult::kOk &&
                  cancel.Snapshot().counts[0] == 1 && !cancel.Snapshot().transaction_pending,
              "safe pre-pulse cancellation did not restore stock");
        ++count;

        MemoryInventoryBackend uncertain_backend;
        smv::InventoryStore uncertain(uncertain_backend);
        Check(uncertain.Provision(Counts(1)) == smv::InventoryResult::kOk,
              "uncertain fixture provisioning failed");
        const auto uncertain_token = uncertain.Reserve(10, 0, uncertain.Snapshot().revision);
        smv::InventoryStore uncertain_reboot(uncertain_backend);
        Check(uncertain_token.has_value() &&
                  uncertain_reboot.Load() == smv::InventoryResult::kPendingTransaction,
              "uncertain relay outcome was auto-cleared");
        ++count;

        MemoryInventoryBackend corrupt_pending_backend = uncertain_backend;
        corrupt_pending_backend.blobs["snapshot_b"].back() ^= 1;
        smv::InventoryStore corrupt_pending_reboot(corrupt_pending_backend);
        Check(corrupt_pending_reboot.Load() == smv::InventoryResult::kCorrupt &&
                  !corrupt_pending_reboot.Snapshot().available,
              "corrupt newest pending snapshot rolled back to pre-reservation stock");
        ++count;

        std::cout << "HOST_INVENTORY_STORE_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
