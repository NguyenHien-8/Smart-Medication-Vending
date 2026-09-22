#pragma once

#include "inventory_store.h"

#include <nvs.h>

#include <string>

namespace smv {
class NvsInventoryBackend : public InventoryBackend {
public:
    explicit NvsInventoryBackend(const char* storage_namespace = "smv_inventory");
    ~NvsInventoryBackend() override;

    NvsInventoryBackend(const NvsInventoryBackend&) = delete;
    NvsInventoryBackend& operator=(const NvsInventoryBackend&) = delete;

    bool valid() const { return valid_; }
    bool ReadBlob(std::string_view key, std::vector<uint8_t>& value) override;
    bool WriteBlob(std::string_view key, std::span<const uint8_t> value) override;

private:
    nvs_handle_t handle_ = 0;
    bool valid_ = false;
};
}  // namespace smv
