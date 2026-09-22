#include "nvs_inventory_backend.h"

#include <esp_log.h>

namespace smv {
namespace {
constexpr char kTag[] = "smv_inventory";

bool ValidKey(std::string_view key) { return !key.empty() && key.size() <= 15; }
}  // namespace

NvsInventoryBackend::NvsInventoryBackend(const char* storage_namespace) {
    const esp_err_t error = nvs_open(storage_namespace, NVS_READWRITE, &handle_);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "open failed: %s", esp_err_to_name(error));
        return;
    }
    valid_ = true;
}

NvsInventoryBackend::~NvsInventoryBackend() {
    if (valid_)
        nvs_close(handle_);
}

bool NvsInventoryBackend::ReadBlob(std::string_view key, std::vector<uint8_t>& value) {
    value.clear();
    if (!valid_ || !ValidKey(key))
        return false;
    const std::string key_text(key);
    size_t size = 0;
    esp_err_t error = nvs_get_blob(handle_, key_text.c_str(), nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND)
        return false;
    if (error != ESP_OK || size == 0 || size > 256) {
        ESP_LOGE(kTag, "read size failed: %s", esp_err_to_name(error));
        return false;
    }
    value.resize(size);
    error = nvs_get_blob(handle_, key_text.c_str(), value.data(), &size);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "read failed: %s", esp_err_to_name(error));
        value.clear();
        return false;
    }
    value.resize(size);
    return true;
}

bool NvsInventoryBackend::WriteBlob(std::string_view key, std::span<const uint8_t> value) {
    if (!valid_ || !ValidKey(key) || value.empty() || value.size() > 256)
        return false;
    const std::string key_text(key);
    esp_err_t error = nvs_set_blob(handle_, key_text.c_str(), value.data(), value.size());
    if (error == ESP_OK)
        error = nvs_commit(handle_);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "write failed: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}
}  // namespace smv
