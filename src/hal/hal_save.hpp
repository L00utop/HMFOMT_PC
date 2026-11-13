#pragma once

#include "gba_types.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace hm::hal {

class GbaBus;

class HalSave {
public:
    explicit HalSave(std::filesystem::path save_path = "saves/fomt.sav");

    void set_path(std::filesystem::path save_path);
    const std::filesystem::path& path() const { return save_path_; }

    bool load_into_bus(GbaBus& bus);
    bool flush_from_bus(GbaBus& bus, bool force = false);

    // Generic atomic binary persistence for PC-extended farm/coop state
    static bool write_atomic(const std::filesystem::path& file_path, const void* data, std::size_t size);
    static bool read_file(const std::filesystem::path& file_path, std::vector<u8>& out_data);

private:
    std::filesystem::path save_path_;
};

} // namespace hm::hal
