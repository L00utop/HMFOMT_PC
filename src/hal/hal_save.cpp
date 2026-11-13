#include "hal_save.hpp"
#include "gba_bus.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace hm::hal {

HalSave::HalSave(std::filesystem::path save_path)
    : save_path_(std::move(save_path)) {
}

void HalSave::set_path(std::filesystem::path save_path) {
    save_path_ = std::move(save_path);
}

bool HalSave::write_atomic(const std::filesystem::path& file_path, const void* data, std::size_t size) {
    if (!data && size > 0) return false;
    std::error_code ec;
    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path(), ec);
        if (ec) return false;
    }

    auto tmp_path = file_path;
    tmp_path += ".tmp";

    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        if (size > 0) {
            ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            if (!ofs.good()) return false;
        }
        ofs.flush();
        if (!ofs.good()) return false;
    }

    std::filesystem::rename(tmp_path, file_path, ec);
    if (ec) {
        std::filesystem::remove(file_path, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, file_path, ec);
        if (ec) return false;
    }
    return true;
}

bool HalSave::read_file(const std::filesystem::path& file_path, std::vector<u8>& out_data) {
    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;
    const std::streamsize sz = ifs.tellg();
    if (sz < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out_data.resize(static_cast<std::size_t>(sz));
    if (sz > 0) {
        if (!ifs.read(reinterpret_cast<char*>(out_data.data()), sz)) {
            out_data.clear();
            return false;
        }
    }
    return true;
}

bool HalSave::load_into_bus(GbaBus& bus) {
    std::memset(bus.save_ram(), 0xFF, SAVE_SIZE);
    std::vector<u8> raw;
    if (!read_file(save_path_, raw)) {
        bus.clear_save_dirty();
        return false;
    }
    const std::size_t copy_bytes = std::min(raw.size(), SAVE_SIZE);
    if (copy_bytes > 0) {
        std::memcpy(bus.save_ram(), raw.data(), copy_bytes);
    }
    bus.clear_save_dirty();
    return true;
}

bool HalSave::flush_from_bus(GbaBus& bus, bool force) {
    if (!force && !bus.save_dirty()) {
        return true;
    }
    if (!write_atomic(save_path_, bus.save_ram(), SAVE_SIZE)) {
        return false;
    }
    bus.clear_save_dirty();
    return true;
}

} // namespace hm::hal
