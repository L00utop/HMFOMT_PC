#include "game/ewram_coop_bridge.hpp"
#include "game/fomt_rom_assets.hpp"
#include "hal/gba_bus.hpp"
#include "net/net_coop.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace hm::game;
using namespace hm::hal;
using namespace hm::net;

namespace {

std::string find_baserom() {
    for (const char* candidate : {"baserom.gba", "../baserom.gba", "../../baserom.gba"}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

void setup_synthetic_ewram(GbaBus& bus, u16 map_id, i16 px, i16 py, u8 facing, u16 anim_id, u16 date_u16, u16 time_u16) {
    constexpr u32 SCENE_ADDR  = 0x02002074u;
    constexpr u32 GS_ADDR     = 0x020025D8u;
    constexpr u32 PLAYER_ADDR = 0x02039044u;

    bus.write32(EwramCoopBridge::PTR_GAME_SCENE, SCENE_ADDR);
    bus.write32(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_GAMESTATE, GS_ADDR);
    bus.write32(SCENE_ADDR + 4u, GS_ADDR);

    bus.write16(GS_ADDR + EwramCoopBridge::OFF_GS_DATE_U16, date_u16);
    bus.write16(GS_ADDR + EwramCoopBridge::OFF_GS_TIME_U16, time_u16);
    bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMER + 0x54u, 0xFFFF0005u);

    bus.write32(PLAYER_ADDR + 0x14u, EwramCoopBridge::PLAYER_VTABLE);
    bus.write16(PLAYER_ADDR + 0x04u, map_id);
    bus.write32(PLAYER_ADDR + 0x08u, static_cast<u32>(static_cast<i32>(px) << 16));
    bus.write32(PLAYER_ADDR + 0x0Cu, static_cast<u32>(static_cast<i32>(py) << 16));
    bus.write8(PLAYER_ADDR + 0x20u, facing);
    bus.write16(PLAYER_ADDR + 0x22u, anim_id);
}

} // namespace

int main() {
    GbaBus host_bus;
    GbaBus client_bus;
    host_bus.reset();
    client_bus.reset();

    // Host: 11:12 AM (11 | (12 << 5) = 0x018B) with top bits 0x7800, Client: 6:00 AM with top bits 0xA800
    constexpr u32 GS_ADDR = 0x020025D8u;
    setup_synthetic_ewram(host_bus, 2u, 320, 180, 3u, 402u, 0x0425u, 0x798Bu);
    setup_synthetic_ewram(client_bus, 2u, 300, 170, 2u, 338u, 0x0001u, 0xA806u);

    // Seed specific Farm Field<43, 25> plot in Host EWRAM before Client connects
    host_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 10u * 4u, 0x00031406u);
    host_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 999u * 4u, 0x00120105u);

    NetCoopSession host_net;
    NetCoopSession client_net;
    EwramCoopBridge host_bridge;
    EwramCoopBridge client_bridge;

    host_bridge.reset();
    client_bridge.reset();

    assert(host_net.start_host(0));
    const std::string addr = "127.0.0.1:" + std::to_string(host_net.bound_port());
    assert(client_net.connect_client(addr));

    host_bridge.bind(host_net);
    client_bridge.bind(client_net);

    FarmWorld dummy_host_world;
    FarmWorld dummy_client_world;

    for (int t = 0; t < 60; ++t) {
        host_bridge.update_before_frame(host_bus, host_net);
        client_bridge.update_before_frame(client_bus, client_net);
        host_net.tick(dummy_host_world);
        client_net.tick(dummy_client_world);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (client_net.is_connected() &&
            client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 999u * 4u) == 0x00120105u &&
            host_bridge.remote_farmers()[1].active &&
            client_bridge.remote_farmers()[0].active) {
            break;
        }
    }

    // Flush one extra frame after client received all chunks so pending_client_plots_ applies cleanly
    host_bridge.update_before_frame(host_bus, host_net);
    client_bridge.update_before_frame(client_bus, client_net);

    assert(client_net.is_connected());
    assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 10u * 4u) == 0x00031406u);
    assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 999u * 4u) == 0x00120105u);

    // Client time bits [10:0] updated to Host's 0x018B while Client's upper flags 0xA800 stayed intact!
    assert(client_bus.read16(GS_ADDR + EwramCoopBridge::OFF_GS_DATE_U16) == 0x0425u);
    assert(client_bus.read16(GS_ADDR + EwramCoopBridge::OFF_GS_TIME_U16) == (0xA800u | 0x018Bu));

    // Bidirectional Farmer pose check
    const auto& p2_on_host = host_bridge.remote_farmers()[1];
    assert(p2_on_host.active && p2_on_host.map_id == 2u && p2_on_host.x == 300 && p2_on_host.y == 170 && p2_on_host.anim_id == 338u);
    const auto& p1_on_client = client_bridge.remote_farmers()[0];
    assert(p1_on_client.active && p1_on_client.map_id == 2u && p1_on_client.x == 320 && p1_on_client.y == 180 && p1_on_client.anim_id == 402u);

    // Mutate a Field<43, 25> plot on Client -> verify Host receives live EWRAM FieldDelta!
    client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 55u * 4u, 0x00020309u);
    for (int t = 0; t < 25; ++t) {
        client_bridge.update_before_frame(client_bus, client_net);
        host_bridge.update_before_frame(host_bus, host_net);
        client_net.tick(dummy_client_world);
        host_net.tick(dummy_host_world);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 55u * 4u) == 0x00020309u) {
            break;
        }
    }
    host_bridge.update_before_frame(host_bus, host_net);
    assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + 55u * 4u) == 0x00020309u);

    // Verify clean remote P2 OAM injection + non-destructive restore when baserom.gba assets exist
    const std::string rom_path = find_baserom();
    if (!rom_path.empty()) {
        assert(host_bus.load_rom_file(rom_path.c_str()));
        FomtRomAssets assets;
        assert(assets.load_from_rom(host_bus.rom().data(), host_bus.rom().size()));

        // Simulate ROM occupying OAM slot 0 at top-left, leaving slots 1..127 hidden
        for (u32 s = 0; s < 128u; ++s) {
            host_bus.write16(ADDR_OAM_BASE + s * 8u, (s == 0u) ? 0x0020u : 0x0200u);
        }
        host_bus.io_write16(reg::BG1HOFS, 200u);
        host_bus.io_write16(reg::BG1VOFS, 100u);

        std::vector<u8> orig_oam(host_bus.oam(), host_bus.oam() + OAM_SIZE);
        std::vector<u8> orig_pal(host_bus.pal_ram(), host_bus.pal_ram() + PAL_SIZE);
        std::vector<u8> orig_vram(host_bus.vram(), host_bus.vram() + VRAM_SIZE);

        host_bridge.inject_remote_oam_before_render(host_bus, assets, 0u);
        int active_injected = 0;
        for (u32 s = 1; s < 128u; ++s) {
            const u16 a0 = host_bus.read16(ADDR_OAM_BASE + s * 8u);
            if ((a0 & 0x0300u) != 0x0200u) {
                ++active_injected;
            }
        }
        assert(active_injected > 0);

        host_bridge.restore_oam_after_render(host_bus);
        assert(std::memcmp(orig_oam.data(), host_bus.oam(), OAM_SIZE) == 0);
        assert(std::memcmp(orig_pal.data(), host_bus.pal_ram(), PAL_SIZE) == 0);
        assert(std::memcmp(orig_vram.data(), host_bus.vram(), VRAM_SIZE) == 0);

        // Regression test for FoMT 8x8 BG1HOFS/VOFS wrap vs true EWRAM camera scrolling:
        // Keep Client (P2) stationary at world (180, 140) while Host (P1) scrolls rightward across tile boundaries.
        constexpr u32 SCENE_ADDR  = 0x02002074u;
        constexpr u32 PLAYER_ADDR = 0x02039044u;
        constexpr u32 UNK10_ADDR  = 0x0203911Cu;
        constexpr u32 GO_ADDR     = 0x0202C0FCu;
        constexpr u32 CAM_ADDR    = 0x02005CF8u;
        constexpr u32 DECOY_ADDR  = 0x02030000u;

        // Place an earlier decoy with PLAYER_VTABLE at lower EWRAM address to confirm scene+0x3B10->go+0x08 wins
        host_bus.write32(DECOY_ADDR + 0x14u, EwramCoopBridge::PLAYER_VTABLE);
        host_bus.write16(DECOY_ADDR + 0x04u, 2u);
        host_bus.write32(DECOY_ADDR + 0x08u, static_cast<u32>(900 << 16));
        host_bus.write32(DECOY_ADDR + 0x0Cu, static_cast<u32>(900 << 16));

        host_bus.write32(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_MAPCTRL, GO_ADDR);
        host_bus.write32(GO_ADDR + 0x04u, CAM_ADDR);
        host_bus.write32(GO_ADDR + 0x08u, PLAYER_ADDR);
        host_bus.write32(PLAYER_ADDR + 0x10u, UNK10_ADDR);
        host_bus.write32(UNK10_ADDR + 0x00u, PLAYER_ADDR);
        host_bus.write32(UNK10_ADDR + 0x08u, GO_ADDR);

        auto decode_oam_x = [](const GbaBus& b, u32 slot) -> int {
            const u16 a1 = b.read16(ADDR_OAM_BASE + slot * 8u + 2u);
            int x = static_cast<int>(a1 & 0x1FFu);
            if (x >= 256) x -= 512;
            return x;
        };
        auto decode_oam_y = [](const GbaBus& b, u32 slot) -> int {
            const u16 a0 = b.read16(ADDR_OAM_BASE + slot * 8u);
            int y = static_cast<int>(a0 & 0xFFu);
            if (y >= 160) y -= 256;
            return y;
        };

        client_bus.write32(0x02039044u + 0x08u, static_cast<u32>(180 << 16));
        client_bus.write32(0x02039044u + 0x0Cu, static_cast<u32>(140 << 16));
        for (int t = 0; t < 20; ++t) {
            client_bridge.update_before_frame(client_bus, client_net);
            host_bridge.update_before_frame(host_bus, host_net);
            client_net.tick(dummy_client_world);
            host_net.tick(dummy_host_world);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(host_bridge.remote_farmers()[1].x == 180 && host_bridge.remote_farmers()[1].y == 140);

        // Frame A: Host at (220, 140), cam = (100, 60), BG1HOFS = 4 (100 % 8), BG1VOFS = 4 (60 % 8)
        host_bus.write32(PLAYER_ADDR + 0x08u, static_cast<u32>(220 << 16));
        host_bus.write32(PLAYER_ADDR + 0x0Cu, static_cast<u32>(140 << 16));
        host_bus.write32(CAM_ADDR + 0x08u, static_cast<u32>(100 << 16));
        host_bus.write32(CAM_ADDR + 0x0Cu, static_cast<u32>(60 << 16));
        host_bus.io_write16(reg::BG1HOFS, 4u);
        host_bus.io_write16(reg::BG1VOFS, 4u);
        host_bridge.update_before_frame(host_bus, host_net);
        assert(host_bridge.local_snapshot().player_ptr == PLAYER_ADDR);
        assert(host_bridge.local_snapshot().x == 220 && host_bridge.local_snapshot().y == 140);
        host_bridge.inject_remote_oam_before_render(host_bus, assets, 0u);
        const int x_a = decode_oam_x(host_bus, 1u);
        const int y_a = decode_oam_y(host_bus, 1u);
        host_bridge.restore_oam_after_render(host_bus);

        // Frame B: Host walked RIGHT to (280, 140), cam = (160, 60), BG1HOFS wrapped to 0 (160 % 8).
        // Stationary P2 at world (180, 140) MUST move LEFT on screen by exact 60 px (x_b == x_a - 60).
        host_bus.write32(PLAYER_ADDR + 0x08u, static_cast<u32>(280 << 16));
        host_bus.write32(CAM_ADDR + 0x08u, static_cast<u32>(160 << 16));
        host_bus.io_write16(reg::BG1HOFS, 0u);
        host_bridge.update_before_frame(host_bus, host_net);
        host_bridge.inject_remote_oam_before_render(host_bus, assets, 0u);
        const int x_b = decode_oam_x(host_bus, 1u);
        const int y_b = decode_oam_y(host_bus, 1u);
        assert(x_b == x_a - 60);
        assert(y_b == y_a);
        host_bridge.restore_oam_after_render(host_bus);

        // Frame C: EWRAM cam steps 1px to 161 while HW BG1HOFS is still 0 from prev VBlank -> aligned to 160.
        host_bus.write32(PLAYER_ADDR + 0x08u, static_cast<u32>(281 << 16));
        host_bus.write32(CAM_ADDR + 0x08u, static_cast<u32>(161 << 16));
        host_bridge.update_before_frame(host_bus, host_net);
        host_bridge.inject_remote_oam_before_render(host_bus, assets, 0u);
        assert(decode_oam_x(host_bus, 1u) == x_a - 60);
        host_bridge.restore_oam_after_render(host_bus);

        // Frame D: VBlank latches BG1HOFS = 1 -> now renders at exact cam 161 (x_a - 61).
        host_bus.io_write16(reg::BG1HOFS, 1u);
        host_bridge.update_before_frame(host_bus, host_net);
        host_bridge.inject_remote_oam_before_render(host_bus, assets, 0u);
        assert(decode_oam_x(host_bus, 1u) == x_a - 61);
        host_bridge.restore_oam_after_render(host_bus);

        // Ambient PaletteTint fitting + Drop Shadow OAM cloning verification:
        // Blend Pete's raw palette toward FoMT Morning tint 0x1BAA with weight w=6/32 (func_03000C2C exact math)
        const u8* raw_pal = assets.farmer_arch().pal_ptr(0u);
        assert(raw_pal != nullptr);
        // Verify fit_palette_tint across Morning (0x1BAA), Evening (0x6A32), Night (0x2529), & Indoor Identity
        auto check_ambient_blend = [&](u16 tint_bgr, u16 weight_of_32) {
            std::array<u8, 32> blended{};
            const u16 tr = tint_bgr & 31u, tg = (tint_bgr >> 5) & 31u, tb = (tint_bgr >> 10) & 31u;
            const u16 w_raw = static_cast<u16>(32u - weight_of_32);
            for (int i = 0; i < 16; ++i) {
                const u16 rc = static_cast<u16>(raw_pal[i * 2] | (static_cast<u16>(raw_pal[i * 2 + 1]) << 8));
                if (i == 0) {
                    blended[0] = raw_pal[0];
                    blended[1] = raw_pal[1];
                    continue;
                }
                const u16 r = ((w_raw * (rc & 31u) + weight_of_32 * tr) >> 5) & 31u;
                const u16 g = ((w_raw * ((rc >> 5) & 31u) + weight_of_32 * tg) >> 5) & 31u;
                const u16 b = ((w_raw * ((rc >> 10) & 31u) + weight_of_32 * tb) >> 5) & 31u;
                const u16 lc = static_cast<u16>(r | (g << 5) | (b << 10));
                blended[i * 2] = static_cast<u8>(lc & 0xFFu);
                blended[i * 2 + 1] = static_cast<u8>(lc >> 8);
            }
            float mse = 1e9f;
            const PaletteTint t = fit_palette_tint(raw_pal, blended.data(), &mse);
            assert(!t.is_identity);
            assert(mse < 0.35f);
            return std::pair<std::array<u8, 32>, PaletteTint>{blended, t};
        };
        const auto [morning_pal, fit_t] = check_ambient_blend(0x1BAAu, 6u);
        check_ambient_blend(0x6A32u, 12u); // Evening
        check_ambient_blend(0x2529u, 16u); // Night
        float fit_mse = 1e9f;
        assert(fit_palette_tint(raw_pal, raw_pal, &fit_mse).is_identity && fit_mse == 0.0f);

        // Place simulated local Pete body OAM in slot 0 (using bank 3 loaded with morning_pal)
        // and local Pete's semi-transparent shadow OAM in slot 1 (using bank 4) right under (281, 140)
        for (u32 s = 0; s < 128u; ++s) {
            host_bus.write16(ADDR_OAM_BASE + s * 8u, 0x0200u);
        }
        std::memcpy(host_bus.pal_ram() + 512u + 3u * 32u, morning_pal.data(), 32u);
        const int local_sx = 281 - 161; // 120
        const int local_sy = 140 - 60;  // 80
        // Body: 16x32 normal 4bpp @ (local_sx - 8, local_sy - 28), bank 3, prio 1
        host_bus.write16(ADDR_OAM_BASE + 0u, static_cast<u16>(0x8000u | ((local_sy - 28) & 0xFFu)));
        host_bus.write16(ADDR_OAM_BASE + 2u, static_cast<u16>(0x8000u | ((local_sx - 8) & 0x1FFu)));
        host_bus.write16(ADDR_OAM_BASE + 4u, static_cast<u16>(0x3000u | (1u << 10) | 10u));
        // Shadow: 16x8 semi-transparent (obj_mode=1 -> 0x0400) @ (local_sx - 8, local_sy - 3), bank 4, tile 42
        host_bus.write16(ADDR_OAM_BASE + 8u, static_cast<u16>(0x4400u | ((local_sy - 3) & 0xFFu)));
        host_bus.write16(ADDR_OAM_BASE + 10u, static_cast<u16>(0x4000u | ((local_sx - 8) & 0x1FFu)));
        host_bus.write16(ADDR_OAM_BASE + 12u, static_cast<u16>(0x4000u | (1u << 10) | 42u));

        host_bridge.inject_remote_oam_before_render(host_bus, assets, 0u);
        // Find injected P2 body OAM and verify its live PALRAM bank matches morning-tinted P2 crimson!
        int found_p2_shadows = 0;
        u8 p2_body_bank = 0xFFu;
        for (u32 s = 2; s < 128u; ++s) {
            const u16 a0 = host_bus.read16(ADDR_OAM_BASE + s * 8u);
            if ((a0 & 0x0300u) == 0x0200u) continue;
            const u16 a2 = host_bus.read16(ADDR_OAM_BASE + s * 8u + 4u);
            if ((a0 & 0x0C00u) == 0x0400u) {
                ++found_p2_shadows;
                assert((a2 & 0x03FFu) == 42u);
                assert(decode_oam_x(host_bus, s) == (180 - 161 - 8));
                assert(decode_oam_y(host_bus, s) == (140 - 60 - 3));
            } else if (p2_body_bank == 0xFFu) {
                p2_body_bank = static_cast<u8>((a2 >> 12) & 0xFu);
            }
        }
        assert(found_p2_shadows == 1);
        assert(p2_body_bank != 0xFFu && p2_body_bank != 3u && p2_body_bank != 4u);
        // Check a skin color & a crimson-remapped hat color in P2's live palette bank match fit_t(recolor(raw)):
        const u16 p2_anim = static_cast<u16>(338u + 2u);
        const u16 p2_cel_id = assets.farmer_arch().cel_for_anim(p2_anim, 0u);
        const auto p2_cel = assets.farmer_arch().cel(p2_cel_id);
        const u8* p2_raw_pal = assets.farmer_arch().pal_ptr(p2_cel.pal_i);
        assert(p2_raw_pal != nullptr);
        for (int i = 1; i < 16; ++i) {
            const u16 rc = static_cast<u16>(p2_raw_pal[i * 2] | (static_cast<u16>(p2_raw_pal[i * 2 + 1]) << 8));
            const u16 expected = fit_t.apply(recolor_coop_bgr555(rc, 1u));
            const u16 actual = host_bus.read16(ADDR_PAL_BASE + 512u + static_cast<u32>(p2_body_bank) * 32u + static_cast<u32>(i * 2));
            assert(actual == expected);
        }
        host_bridge.restore_oam_after_render(host_bus);
    }

    // Collaborative World (House Upgrades, Gotz Build, Lumber, Horse, Coop, Barn, Animals, Dog)
    // vs Strictly Separate Personal Money (Gold), ShippingBin, ToolChest, & Farmer Inventory/Stats!
    {
        constexpr u32 SCENE_ADDR = 0x02002074u;
        constexpr u32 GS_ADDR    = 0x020025D8u;

        // Wire GameScene clock pointer so sub_frame sync is active
        host_bus.write32(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_CLOCK_PTR, GS_ADDR + EwramCoopBridge::OFF_GS_DATE_U16);
        client_bus.write32(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_CLOCK_PTR, GS_ADDR + EwramCoopBridge::OFF_GS_DATE_U16);
        host_bus.write8(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_SUBFRAME, 61u);
        host_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_WEATHER_TODAY, 1u);    // Rain today
        host_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_WEATHER_TOMORROW, 2u); // Snow tomorrow

        // Distinct per-player personal data MUST remain isolated
        host_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_GOLD, 12345u);
        client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_GOLD, 777u);
        host_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMER + 0x10u, 0xCAFEBABEu);
        client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMER + 0x10u, 0x00000105u);
        host_bus.write32(GS_ADDR + 0x0054u, 900u);  // Host ShippingBin pending cash
        client_bus.write32(GS_ADDR + 0x0054u, 250u); // Client ShippingBin pending cash
        host_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0x18Cu, 0x11112222u); // Host ToolChest
        client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0x18Cu, 0x33334444u); // Client ToolChest

        // Step clock/weather/sub_frame sync Host -> Client
        for (int t = 0; t < 15; ++t) {
            host_bridge.update_before_frame(host_bus, host_net);
            client_bridge.update_before_frame(client_bus, client_net);
            host_net.tick(dummy_host_world);
            client_net.tick(dummy_client_world);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client_bridge.update_before_frame(client_bus, client_net);
        assert(client_bus.read8(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_SUBFRAME) == 61u);
        assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_WEATHER_TODAY) == 1u);
        assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_WEATHER_TOMORROW) == 2u);

        // Client upgrades House, buys Kitchen + Knife, sets Gotz construction, Coop/Barn animals & Dog!
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0u, 0x02u); // upgrade_level = 2
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 4u, 0x02u); // has_kitchen
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 5u, 0x01u); // knife
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_GOTZ_BUILD + 0u, 0xB1u); // House build ordered, 3 days
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_GOTZ_BUILD + 1u, 0x01u);
        client_bus.write16(GS_ADDR + EwramCoopBridge::OFF_GS_FARM_HDR + 0x10u, 0x0590u); // Lumber + has_horse
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 0u, 0x01u);     // Coop upgrade
        client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 0x18u, 0x4348494Bu); // Chicken
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_BARN + 0u, 0x01u);     // Barn upgrade
        client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_BARN + 0x2Cu, 0x4D4F4F31u); // Cow
        client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_DOG + 0x08u, 0x444F4731u);  // Dog data

        for (int t = 0; t < 25; ++t) {
            client_bridge.update_before_frame(client_bus, client_net);
            host_bridge.update_before_frame(host_bus, host_net);
            client_net.tick(dummy_client_world);
            host_net.tick(dummy_host_world);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        host_bridge.update_before_frame(host_bus, host_net);

        assert((host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0u) & 0x03u) == 2u);
        assert((host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 4u) & 0x02u) != 0u);
        assert((host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 5u) & 0x01u) != 0u);
        assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_GOTZ_BUILD + 0u) == 0xB1u);
        assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_GOTZ_BUILD + 1u) == 0x01u);
        assert(host_bus.read16(GS_ADDR + EwramCoopBridge::OFF_GS_FARM_HDR + 0x10u) == 0x0590u);
        assert((host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 0u) & 0x01u) == 1u);
        assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 0x18u) == 0x4348494Bu);
        assert((host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_BARN + 0u) & 0x01u) == 1u);
        assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_BARN + 0x2Cu) == 0x4D4F4F31u);
        assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_DOG + 0x08u) == 0x444F4731u);

        // Monotonic merge guard: a style update carrying level=0 cannot downgrade Host house level 2!
        client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0u, 0x08u); // style=2, level bits=0
        for (int t = 0; t < 15; ++t) {
            client_bridge.update_before_frame(client_bus, client_net);
            host_bridge.update_before_frame(host_bus, host_net);
            client_net.tick(dummy_client_world);
            host_net.tick(dummy_host_world);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        host_bridge.update_before_frame(host_bus, host_net);
        assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0u) == 0x0Au); // style=2 preserved AND level=2 kept!

        // Personal Gold, Farmer, ShippingBin, and ToolChest MUST still be completely separate!
        assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_GOLD) == 12345u);
        assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_GOLD) == 777u);
        assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMER + 0x10u) == 0xCAFEBABEu);
        assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMER + 0x10u) == 0x00000105u);
        assert(host_bus.read32(GS_ADDR + 0x0054u) == 900u);
        assert(client_bus.read32(GS_ADDR + 0x0054u) == 250u);
        assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0x18Cu) == 0x11112222u);
        assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMHOUSE + 0x18Cu) == 0x33334444u);
    }

    // Verify held item icon mapping, ambient-tinted remote held item OAM, gfx_mode=0 tile=4 shadow cloning,
    // real-time Farm MapRenderer tile/collision refresh on field deltas, and OFF_GS_WILD_FORAGE sync.
    {
        const std::string rom_path2 = find_baserom();
        if (!rom_path2.empty()) {
            assert(host_bus.load_rom_file(rom_path2.c_str()));
            assert(client_bus.load_rom_file(rom_path2.c_str()));
            FomtRomAssets assets2;
            assert(assets2.load_from_rom(host_bus.rom().data(), host_bus.rom().size()));

            // 1. HeldItem -> icon ID table mapping
            u16 icon_id = 0u;
            assert(!assets2.held_item_icon_id(0xFFFF0005u, icon_id)); // KIND_5 empty
            assert(assets2.held_item_icon_id(0xFF300001u, icon_id) && icon_id == 471u);   // WEEDS
            assert(assets2.held_item_icon_id(0xFF310001u, icon_id) && icon_id == 428u);   // STONES
            assert(assets2.held_item_icon_id(0x002F0000u, icon_id) && icon_id == 65u);    // BLUE_GRASS
            assert(assets2.held_item_icon_id(0x00000008u, icon_id) && icon_id == 0x160u); // wrapped gift
            assert(assets2.held_item_icon_id(0xFFFF0004u, icon_id) && icon_id == 0x35u);  // basket

            SpriteArchiveView farmer_arch;
            SpriteArchiveView item_arch;
            assert(farmer_arch.parse(host_bus.rom().data() + FomtRomAssets::FARMER_ARCH_OFF,
                                     host_bus.rom().size() - FomtRomAssets::FARMER_ARCH_OFF));
            assert(item_arch.parse(host_bus.rom().data() + FomtRomAssets::ITEM_ICON_ARCH_OFF,
                                   host_bus.rom().size() - FomtRomAssets::ITEM_ICON_ARCH_OFF));

            constexpr u32 SCENE_ADDR  = 0x02002074u;
            constexpr u32 GS_ADDR     = 0x020025D8u;
            constexpr u32 PLAYER_ADDR = 0x02039044u;
            constexpr u32 GO_ADDR     = 0x0202C0FCu;
            constexpr u32 CAM_ADDR    = 0x02005CF8u;

            // Keep both players on Farm (map 0) with camera at (0, 0)
            host_bus.write32(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_MAPCTRL, GO_ADDR);
            host_bus.write32(GO_ADDR + 0x04u, CAM_ADDR);
            host_bus.write32(GO_ADDR + 0x08u, PLAYER_ADDR);
            for (u32 off = 0u; off < 0x40u; off += 4u) {
                host_bus.write32(CAM_ADDR + off, 0u);
            }
            host_bus.io_write16(reg::BG1HOFS, 0u);
            host_bus.io_write16(reg::BG1VOFS, 0u);

            const i16 local_sx = 64;
            const i16 local_sy = 96;
            const i16 p2_sx    = 160;
            const i16 p2_sy    = 96;

            const u16 idle_anim = FomtRomAssets::select_pete_anim(Facing::Down, false, 0u, 0u);
            host_bus.write16(PLAYER_ADDR + 0x04u, 0u);
            host_bus.write32(PLAYER_ADDR + 0x08u, static_cast<u32>(static_cast<i32>(local_sx) << 16));
            host_bus.write32(PLAYER_ADDR + 0x0Cu, static_cast<u32>(static_cast<i32>(local_sy) << 16));
            host_bus.write8(PLAYER_ADDR + 0x20u, 0u);
            host_bus.write16(PLAYER_ADDR + 0x22u, idle_anim);

            client_bus.write16(PLAYER_ADDR + 0x04u, 0u);
            client_bus.write32(PLAYER_ADDR + 0x08u, static_cast<u32>(static_cast<i32>(p2_sx) << 16));
            client_bus.write32(PLAYER_ADDR + 0x0Cu, static_cast<u32>(static_cast<i32>(p2_sy) << 16));
            client_bus.write8(PLAYER_ADDR + 0x20u, 0u);
            client_bus.write16(PLAYER_ADDR + 0x22u, idle_anim);

            // Reset OAM, OBJ PALRAM, and OBJ VRAM cleanly
            for (u32 s = 0u; s < 128u; ++s) {
                host_bus.write16(ADDR_OAM_BASE + s * 8u, 0x0200u);
                host_bus.write16(ADDR_OAM_BASE + s * 8u + 2u, 0u);
                host_bus.write16(ADDR_OAM_BASE + s * 8u + 4u, 0u);
            }
            std::memset(host_bus.pal_ram() + 0x200u, 0, 512u);
            std::memset(host_bus.vram() + 0x10000u, 0, 0x8000u);

            // Place morning-tinted local Pete OAM pieces in bank 3 starting at tile 64
            const u16 pcel_id = farmer_arch.cel_for_anim(idle_anim, 0u);
            const SpriteArchiveView::Cel pcel = farmer_arch.cel(pcel_id);
            const u8* raw_farmer_pal = farmer_arch.pal_ptr(pcel.pal_i);
            assert(raw_farmer_pal != nullptr);

            PaletteTint morning;
            morning.scale = {0.72f, 0.68f, 0.85f};
            morning.offset = {1.0f, 0.0f, 2.0f};
            morning.is_identity = false;

            auto read_u16_le = [](const u8* p) -> u16 {
                return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
            };
            auto write_u16_le = [](u8* p, u16 v) {
                p[0] = static_cast<u8>(v & 0xFFu);
                p[1] = static_cast<u8>((v >> 8) & 0xFFu);
            };

            for (u32 c = 0u; c < 16u; ++c) {
                const u16 raw_c = read_u16_le(raw_farmer_pal + c * 2u);
                write_u16_le(host_bus.pal_ram() + 0x200u + 3u * 32u + c * 2u,
                             (c == 0u) ? 0u : morning.apply(raw_c));
            }
            const u32 rem_farmer_tiles = (pcel.tile_i < farmer_arch.n_tile)
                ? static_cast<u32>(farmer_arch.n_tile - pcel.tile_i) : 0u;
            const u32 copy_farmer_tiles = (rem_farmer_tiles < 64u) ? rem_farmer_tiles : 64u;
            for (u32 t = 0u; t < copy_farmer_tiles; ++t) {
                const u8* src = farmer_arch.tile_ptr(pcel.tile_i + t);
                if (src != nullptr) {
                    std::memcpy(host_bus.vram() + 0x10000u + (64u + t) * 32u, src, 32u);
                }
            }
            for (u16 k = 0u; k < pcel.oam_n; ++k) {
                const SpriteArchiveView::Oam o = farmer_arch.oam(pcel.oam_i + k);
                int dx = static_cast<int>(o.attr1 & 0x1FFu);
                if (dx >= 256) dx -= 512;
                int dy = static_cast<int>(o.attr0 & 0xFFu);
                if (dy >= 128) dy -= 256;
                const u16 a0 = static_cast<u16>((o.attr0 & 0xFF00u) | (static_cast<u16>(local_sy + dy) & 0xFFu));
                const u16 a1 = static_cast<u16>((o.attr1 & 0xFE00u) | (static_cast<u16>(local_sx + dx) & 0x1FFu));
                const u16 a2 = static_cast<u16>((o.attr2 & 0x0C00u) | (3u << 12) | ((64u + (o.attr2 & 0x3FFu)) & 0x3FFu));
                host_bus.write16(ADDR_OAM_BASE + k * 8u + 0u, a0);
                host_bus.write16(ADDR_OAM_BASE + k * 8u + 2u, a1);
                host_bus.write16(ADDR_OAM_BASE + k * 8u + 4u, a2);
            }

            // Local ground shadow: gfx_mode=0 (normal), tile=4, bank=2, 16x8 wide at (local_sx - 8, local_sy - 3)
            const u32 sh_slot = pcel.oam_n;
            std::memset(host_bus.vram() + 0x10000u + 4u * 32u, 0x11, 64u);
            write_u16_le(host_bus.pal_ram() + 0x200u + 2u * 32u + 2u, 0x2108u);
            host_bus.write16(ADDR_OAM_BASE + sh_slot * 8u + 0u,
                             static_cast<u16>(0x4000u | (static_cast<u16>(local_sy - 3) & 0xFFu)));
            host_bus.write16(ADDR_OAM_BASE + sh_slot * 8u + 2u,
                             static_cast<u16>(local_sx - 8) & 0x1FFu);
            host_bus.write16(ADDR_OAM_BASE + sh_slot * 8u + 4u, static_cast<u16>((2u << 12) | 4u));

            // Decoy NPC gfx_mode=0 tile=4 shadow 24px away (must NOT be cloned to P2)
            const u32 decoy_sh_slot = sh_slot + 1u;
            write_u16_le(host_bus.pal_ram() + 0x200u + 5u * 32u + 2u, 0x1084u);
            host_bus.write16(ADDR_OAM_BASE + decoy_sh_slot * 8u + 0u,
                             static_cast<u16>(0x4000u | (static_cast<u16>(local_sy - 3) & 0xFFu)));
            host_bus.write16(ADDR_OAM_BASE + decoy_sh_slot * 8u + 2u,
                             static_cast<u16>(local_sx + 24) & 0x1FFu);
            host_bus.write16(ADDR_OAM_BASE + decoy_sh_slot * 8u + 4u, static_cast<u16>((5u << 12) | 4u));
            const u32 injected_start_slot = decoy_sh_slot + 1u;

            // Pump helper for UDP sync
            auto sync_step = [&]() {
                client_bridge.update_before_frame(client_bus, client_net);
                host_bridge.update_before_frame(host_bus, host_net);
                client_net.tick(dummy_client_world);
                host_net.tick(dummy_host_world);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            };

            auto decode_x = [](const GbaBus& b, u32 s) -> int {
                int x = static_cast<int>(b.read16(ADDR_OAM_BASE + s * 8u + 2u) & 0x1FFu);
                if (x >= 256) x -= 512;
                return x;
            };
            auto decode_y = [](const GbaBus& b, u32 s) -> int {
                int y = static_cast<int>(b.read16(ADDR_OAM_BASE + s * 8u + 0u) & 0xFFu);
                if (y >= 160) y -= 256;
                return y;
            };
            auto iabs = [](int v) -> int { return v < 0 ? -v : v; };

            // 2. Client holds WEEDS (0xFF300001u) -> Host injects morning-tinted Weed OAM before P2 body + 1 shadow
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMER + 0x54u, 0xFF300001u);
            const u16 wcel_id = item_arch.cel_for_anim(471u, 0u);
            const SpriteArchiveView::Cel wcel = item_arch.cel(wcel_id);
            const u8* raw_weed_tile = item_arch.tile_ptr(wcel.tile_i);
            const u8* raw_weed_pal  = item_arch.pal_ptr(wcel.pal_i);
            assert(raw_weed_tile != nullptr && raw_weed_pal != nullptr);

            auto is_weed_oam = [&](u32 s) -> bool {
                const u16 a2 = host_bus.read16(ADDR_OAM_BASE + s * 8u + 4u);
                const u32 tile = a2 & 0x3FFu;
                const int ox = decode_x(host_bus, s);
                const int oy = decode_y(host_bus, s);
                return std::memcmp(host_bus.vram() + 0x10000u + tile * 32u, raw_weed_tile, 32u) == 0 &&
                       iabs(ox - (p2_sx - 7)) <= 2 &&
                       iabs(oy - (p2_sy - 35)) <= 2;
            };

            bool found_weed = false;
            for (int t = 0; t < 25 && !found_weed; ++t) {
                sync_step();
                host_bridge.update_before_frame(host_bus, host_net);
                host_bridge.inject_remote_oam_before_render(host_bus, assets2, 0u);
                for (u32 s = injected_start_slot; s < 128u; ++s) {
                    const u16 a0 = host_bus.read16(ADDR_OAM_BASE + s * 8u + 0u);
                    if ((a0 & 0x0300u) == 0x0200u) continue;
                    if (is_weed_oam(s)) {
                        found_weed = true;
                        break;
                    }
                }
                if (!found_weed) {
                    host_bridge.restore_oam_after_render(host_bus);
                }
            }
            assert(found_weed);

            u32 weed_slot = 999u;
            u32 p2_body_min_slot = 999u;
            int p2_shadow_count = 0;
            int weed_bank = -1;
            const PaletteTint recovered_tint = fit_palette_tint(
                raw_farmer_pal, host_bus.pal_ram() + 0x200u + 3u * 32u);

            for (u32 s = injected_start_slot; s < 128u; ++s) {
                const u16 a0 = host_bus.read16(ADDR_OAM_BASE + s * 8u + 0u);
                if ((a0 & 0x0300u) == 0x0200u) continue;
                const u16 a2 = host_bus.read16(ADDR_OAM_BASE + s * 8u + 4u);
                const u32 tile = a2 & 0x3FFu;
                const u32 bank = (a2 >> 12) & 0xFu;
                const int ox = decode_x(host_bus, s);
                const int oy = decode_y(host_bus, s);

                if (tile == 4u && (a0 & 0x0C00u) == 0u &&
                    iabs(ox - (p2_sx - 8)) <= 2 && iabs(oy - (p2_sy - 3)) <= 2) {
                    ++p2_shadow_count;
                    continue;
                }
                if (is_weed_oam(s)) {
                    if (s < weed_slot) weed_slot = s;
                    weed_bank = static_cast<int>(bank);
                    continue;
                }
                if (iabs(ox - p2_sx) <= 20 && iabs(oy - p2_sy) <= 36) {
                    if (s < p2_body_min_slot) p2_body_min_slot = s;
                }
            }
            assert(weed_slot < p2_body_min_slot);
            assert(p2_shadow_count == 1);
            assert(weed_bank >= 0 && weed_bank != 3);
            for (u32 c = 1u; c < 16u; ++c) {
                const u16 raw_c = read_u16_le(raw_weed_pal + c * 2u);
                const u16 got_c = read_u16_le(host_bus.pal_ram() + 0x200u +
                                              static_cast<u32>(weed_bank) * 32u + c * 2u);
                const u16 exp_fit = recovered_tint.apply(raw_c);
                assert(got_c == exp_fit);
                const u16 exp_morning = morning.apply(raw_c);
                for (int shift = 0; shift <= 10; shift += 5) {
                    const int dc = static_cast<int>((got_c >> shift) & 31u) -
                                   static_cast<int>((exp_morning >> shift) & 31u);
                    assert(iabs(dc) <= 1);
                }
            }
            host_bridge.restore_oam_after_render(host_bus);

            // Clearing HeldItem to KIND_5 empty (0xFFFF0005u) removes Weed OAM while body + 1 shadow stay
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FARMER + 0x54u, 0xFFFF0005u);
            bool weed_cleared = false;
            for (int t = 0; t < 25 && !weed_cleared; ++t) {
                sync_step();
                host_bridge.update_before_frame(host_bus, host_net);
                host_bridge.inject_remote_oam_before_render(host_bus, assets2, 0u);
                bool still_has_weed = false;
                bool has_body = false;
                int shadows = 0;
                for (u32 s = injected_start_slot; s < 128u; ++s) {
                    const u16 a0 = host_bus.read16(ADDR_OAM_BASE + s * 8u + 0u);
                    if ((a0 & 0x0300u) == 0x0200u) continue;
                    const u16 a2 = host_bus.read16(ADDR_OAM_BASE + s * 8u + 4u);
                    const u32 tile = a2 & 0x3FFu;
                    const int ox = decode_x(host_bus, s);
                    const int oy = decode_y(host_bus, s);
                    if (tile == 4u && (a0 & 0x0C00u) == 0u &&
                        iabs(ox - (p2_sx - 8)) <= 2 && iabs(oy - (p2_sy - 3)) <= 2) {
                        ++shadows;
                    } else if (is_weed_oam(s)) {
                        still_has_weed = true;
                    } else if (iabs(ox - p2_sx) <= 20 && iabs(oy - p2_sy) <= 36) {
                        has_body = true;
                    }
                }
                if (!still_has_weed && has_body && shadows == 1) {
                    weed_cleared = true;
                }
                host_bridge.restore_oam_after_render(host_bus);
            }
            assert(weed_cleared);

            // 3. Real-time Farm MapRenderer tile/collision refresh + OFF_GS_WILD_FORAGE sync
            constexpr u32 MR_ADDR  = 0x02010000u;
            constexpr u32 BG0_ADDR = 0x02010100u;
            constexpr u32 BG1_ADDR = 0x02015900u;
            constexpr u32 BG2_ADDR = 0x0201B100u;
            constexpr u32 COL_ADDR = 0x02020900u;
            host_bus.write32(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_MAP_RENDERER, MR_ADDR);
            host_bus.write32(MR_ADDR + 0x04u, 0u); // Map 0 = Farm
            host_bus.write32(MR_ADDR + 0x10u, BG0_ADDR);
            host_bus.write32(MR_ADDR + 0x14u, BG1_ADDR);
            host_bus.write32(MR_ADDR + 0x18u, BG2_ADDR);
            host_bus.write32(MR_ADDR + 0x2Cu, COL_ADDR);
            host_bus.write8(MR_ADDR + 0x29u, 0u);

            constexpr u32 PLOT_IDX = 43u; // plot(fx=0, fy=1) -> tx=34..35, ty=24..25
            auto wait_plot = [&](u32 expected_plot) {
                for (int t = 0; t < 30; ++t) {
                    sync_step();
                    host_bridge.update_before_frame(host_bus, host_net);
                    if (host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u) == expected_plot &&
                        client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u) == expected_plot) {
                        return;
                    }
                }
                assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u) == expected_plot);
                assert(client_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u) == expected_plot);
            };
            auto cell_off = [](u32 tx, u32 ty) -> u32 { return ty * 128u + tx; };

            // Seed plot(0, 1) with Weed (0x00020854u) -> syncs & sets solid col + overlay
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u, 0x00020854u);
            wait_plot(0x00020854u);
            assert(host_bus.read8(MR_ADDR + 0x29u) == 1u);
            for (u32 dy = 0u; dy < 2u; ++dy) {
                for (u32 dx = 0u; dx < 2u; ++dx) {
                    assert((host_bus.read32(COL_ADDR + cell_off(34u + dx, 24u + dy) * 4u) & 1u) == 1u);
                }
            }

            // Pick Weed (0x00020054u) -> clears bg1 (34, 24..25) to 0x93FF93FFu, clears col solid bit, dirty=1
            host_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u, 0x00020054u);
            wait_plot(0x00020054u);
            assert(host_bus.read8(MR_ADDR + 0x29u) == 1u);
            assert(host_bus.read32(BG1_ADDR + cell_off(34u, 24u) * 2u) == 0x93FF93FFu);
            assert(host_bus.read32(BG1_ADDR + cell_off(34u, 25u) * 2u) == 0x93FF93FFu);
            for (u32 dy = 0u; dy < 2u; ++dy) {
                for (u32 dx = 0u; dx < 2u; ++dx) {
                    assert((host_bus.read32(COL_ADDR + cell_off(34u + dx, 24u + dy) * 4u) & 1u) == 0u);
                }
            }

            // Till + water (unk0 = 2 -> 0x00020056u) -> bg0 watered soil 0x934E934Du / 0x9350934Fu
            host_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u, 0x00020056u);
            wait_plot(0x00020056u);
            assert(host_bus.read8(MR_ADDR + 0x29u) == 1u);
            assert(host_bus.read32(BG0_ADDR + cell_off(34u, 24u) * 2u) == 0x934E934Du);
            assert(host_bus.read32(BG0_ADDR + cell_off(34u, 25u) * 2u) == 0x9350934Fu);

            // Boulder place (unk2 = 0x1B, unk8 = 8 -> 0x0002086Cu) -> col solid + action 0x3CC
            host_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u, 0x0002086Cu);
            wait_plot(0x0002086Cu);
            assert(host_bus.read8(MR_ADDR + 0x29u) == 1u);
            for (u32 dy = 0u; dy < 2u; ++dy) {
                for (u32 dx = 0u; dx < 2u; ++dx) {
                    const u32 cval = host_bus.read32(COL_ADDR + cell_off(34u + dx, 24u + dy) * 4u);
                    assert((cval & 1u) == 1u);
                    assert(((cval >> 17) & 0x7FFFu) == 0x3CCu);
                }
            }

            // Boulder smash (unk8 -> 0: 0x0002006Cu) -> col solid bit & action 0x3CC cleared
            host_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_FIELD + PLOT_IDX * 4u, 0x0002006Cu);
            wait_plot(0x0002006Cu);
            assert(host_bus.read8(MR_ADDR + 0x29u) == 1u);
            for (u32 dy = 0u; dy < 2u; ++dy) {
                for (u32 dx = 0u; dx < 2u; ++dx) {
                    const u32 cval = host_bus.read32(COL_ADDR + cell_off(34u + dx, 24u + dy) * 4u);
                    assert((cval & 1u) == 0u);
                    assert(((cval >> 17) & 0x7FFFu) == 0u);
                }
            }

            // Daily wild forageables bitset (OFF_GS_WILD_FORAGE = 0x34C8, 16 bytes) syncs Client -> Host
            client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 3u, 0xA5u);
            client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 12u, 0x5Au);
            for (int t = 0; t < 30; ++t) {
                sync_step();
                host_bridge.update_before_frame(host_bus, host_net);
                if (host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 3u) == 0xA5u &&
                    host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 12u) == 0x5Au) {
                    break;
                }
            }
            assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 3u) == 0xA5u);
            assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 12u) == 0x5Au);

            // 4. Real-time Wild Forageable / Boulder MapRenderer clearing + concurrent same-byte merge
            std::vector<u8> mock_rom(0x110000u, 0u);
            auto rom_w8 = [&](u32 rom_addr, u8 v) {
                mock_rom[rom_addr - 0x08000000u] = v;
            };
            auto rom_w16 = [&](u32 rom_addr, u16 v) {
                const u32 off = rom_addr - 0x08000000u;
                mock_rom[off + 0u] = static_cast<u8>(v & 0xFFu);
                mock_rom[off + 1u] = static_cast<u8>((v >> 8) & 0xFFu);
            };
            auto rom_w32 = [&](u32 rom_addr, u32 v) {
                const u32 off = rom_addr - 0x08000000u;
                mock_rom[off + 0u] = static_cast<u8>(v & 0xFFu);
                mock_rom[off + 1u] = static_cast<u8>((v >> 8) & 0xFFu);
                mock_rom[off + 2u] = static_cast<u8>((v >> 16) & 0xFFu);
                mock_rom[off + 3u] = static_cast<u8>((v >> 24) & 0xFFu);
            };
            auto write_forage_desc = [&](u32 idx, u32 obj_type, u32 map_id, i16 px, i16 py) {
                const u32 addr = 0x081043BCu + idx * 12u;
                rom_w32(addr + 0u, obj_type);
                rom_w32(addr + 4u, map_id);
                rom_w16(addr + 8u, static_cast<u16>(px));
                rom_w16(addr + 10u, static_cast<u16>(py));
            };

            // GetMapData dimensions (128x88) for map_data_id 10 (outdoor) and 0x24 (Coop)
            rom_w16(0x08105EDCu + 10u * 40u + 0x20u, 128u);
            rom_w16(0x08105EDCu + 10u * 40u + 0x22u, 88u);
            rom_w16(0x08105EDCu + 0x24u * 40u + 0x20u, 128u);
            rom_w16(0x08105EDCu + 0x24u * 40u + 0x22u, 88u);

            // Spring Grass #0 (idx=0 -> bit 0), #1 (idx=1 -> bit 1), Mountain Boulder (idx=0x60 -> bit 36 = byte 4 bit 4)
            write_forage_desc(0u, 0u, 10u, 32, 48);
            write_forage_desc(1u, 0u, 10u, 48, 48);
            write_forage_desc(0x60u, 0x14u, 10u, 80, 64);

            // Mock Coop bushel ROM patch (0x08106C8Cu) and X coordinate table (0x081073C4u)
            rom_w8(0x081073C4u + 0u, 5u);
            rom_w8(0x08106C8Cu + 0u, 1u); // w = 1
            rom_w8(0x08106C8Cu + 1u, 1u); // h = 1
            rom_w32(0x08106C8Cu + 8u, 0x08107600u);  // BG1 tile array
            rom_w32(0x08106C8Cu + 16u, 0x08107610u); // collision palette
            rom_w32(0x08106C8Cu + 20u, 0x08107620u); // collision indices
            rom_w16(0x08107600u, 0x1111u);
            rom_w32(0x08107610u + 4u, 0x00020001u);
            rom_w8(0x08107620u, 1u);

            assert(host_bus.load_rom(mock_rom.data(), mock_rom.size()));
            assert(client_bus.load_rom(mock_rom.data(), mock_rom.size()));

            auto setup_mr_on_bus = [&](GbaBus& b, u32 active_map_id, u32 map_data_id) {
                b.write32(SCENE_ADDR + EwramCoopBridge::OFF_SCENE_MAP_RENDERER, MR_ADDR);
                b.write32(MR_ADDR + 0x00u, active_map_id);
                b.write32(MR_ADDR + 0x04u, map_data_id);
                b.write32(MR_ADDR + 0x10u, BG0_ADDR);
                b.write32(MR_ADDR + 0x14u, BG1_ADDR);
                b.write32(MR_ADDR + 0x18u, BG2_ADDR);
                b.write32(MR_ADDR + 0x2Cu, COL_ADDR);
                b.write8(MR_ADDR + 0x29u, 0u);
                b.write8(GS_ADDR + 0x0011u, 0u); // Spring
            };
            setup_mr_on_bus(host_bus, 10u, 10u);
            setup_mr_on_bus(client_bus, 10u, 10u);

            auto fill_forage_rect = [&](GbaBus& b, u32 tx, u32 ty, u32 w, u32 h) {
                for (u32 dy = 0u; dy < h; ++dy) {
                    for (u32 dx = 0u; dx < w; ++dx) {
                        const u32 cell = cell_off(tx + dx, ty + dy);
                        b.write16(BG1_ADDR + cell * 2u, 0x1234u);
                        b.write32(COL_ADDR + cell * 4u, (0x0123u << 17) | 0x00010001u);
                    }
                }
            };
            auto assert_forage_cleared = [&](const GbaBus& b, u32 tx, u32 ty, u32 w, u32 h) {
                assert(b.read8(MR_ADDR + 0x29u) == 1u);
                for (u32 dy = 0u; dy < h; ++dy) {
                    for (u32 dx = 0u; dx < w; ++dx) {
                        const u32 cell = cell_off(tx + dx, ty + dy);
                        assert(b.read16(BG1_ADDR + cell * 2u) == 0x03FFu);
                        assert((b.read8(COL_ADDR + cell * 4u) & 1u) == 0u);
                        const u16 col_hi = b.read16(COL_ADDR + cell * 4u + 2u);
                        assert((col_hi & ~1u) == 0u);
                        assert((col_hi & 1u) == 1u);
                    }
                }
            };

            for (u32 k = 0u; k < 7u; ++k) {
                host_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + k, 0xFFu);
                client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + k, 0xFFu);
            }
            for (int t = 0; t < 22; ++t) {
                sync_step();
            }

            fill_forage_rect(host_bus, 4u, 6u, 2u, 2u);
            fill_forage_rect(host_bus, 6u, 6u, 2u, 2u);
            fill_forage_rect(host_bus, 10u, 8u, 6u, 4u);
            fill_forage_rect(client_bus, 4u, 6u, 2u, 2u);
            fill_forage_rect(client_bus, 6u, 6u, 2u, 2u);
            fill_forage_rect(client_bus, 10u, 8u, 6u, 4u);
            host_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write8(MR_ADDR + 0x29u, 0u);

            // (a) Remote Mountain Boulder smash (byte 4, bit 4: 0xFF -> 0xEF) clears 6x4 patch in Host MapRenderer
            client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 4u, 0xEFu);
            for (int t = 0; t < 30; ++t) {
                sync_step();
                host_bridge.update_before_frame(host_bus, host_net);
                if (host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 4u) == 0xEFu) {
                    break;
                }
            }
            assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 4u) == 0xEFu);
            assert_forage_cleared(host_bus, 10u, 8u, 6u, 4u);
            assert(host_bus.read16(BG1_ADDR + cell_off(4u, 6u) * 2u) == 0x1234u);

            // (b) Concurrent same-frame same-byte pickups (Client clears bit 0, Host clears bit 1 of byte 0) -> 0xFCu on both
            host_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 0u, 0xFEu);
            host_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 0u, 0xFDu);
            for (int t = 0; t < 30; ++t) {
                sync_step();
                host_bridge.update_before_frame(host_bus, host_net);
                if (host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 0u) == 0xFCu &&
                    client_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 0u) == 0xFCu) {
                    break;
                }
            }
            for (int t = 0; t < 4; ++t) {
                sync_step();
            }
            assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 0u) == 0xFCu);
            assert(client_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_WILD_FORAGE + 0u) == 0xFCu);
            assert_forage_cleared(host_bus, 4u, 6u, 2u, 2u);
            assert(host_bus.read16(BG1_ADDR + cell_off(6u, 6u) * 2u) == 0x1234u);
            assert_forage_cleared(client_bus, 6u, 6u, 2u, 2u);
            assert(client_bus.read16(BG1_ADDR + cell_off(4u, 6u) * 2u) == 0x1234u);

            // (c) Coop feed bushel MapRenderer sync (Client places bushel 0 -> Host applies 0x08106C8Cu at (5, 8))
            host_bus.write32(MR_ADDR + 0x04u, 0x24u);
            host_bus.write8(MR_ADDR + 0x29u, 0u);
            client_bus.write32(GS_ADDR + EwramCoopBridge::OFF_GS_COOP, 0x00001001u);
            for (int t = 0; t < 30; ++t) {
                sync_step();
                host_bridge.update_before_frame(host_bus, host_net);
                if (host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_COOP) == 0x00001001u) {
                    break;
                }
            }
            assert(host_bus.read32(GS_ADDR + EwramCoopBridge::OFF_GS_COOP) == 0x00001001u);
            assert(host_bus.read8(MR_ADDR + 0x29u) == 1u);
            assert(host_bus.read16(BG1_ADDR + cell_off(5u, 8u) * 2u) == 0x1111u);
            assert(host_bus.read32(COL_ADDR + cell_off(5u, 8u) * 4u) == 0x00020001u);

            // (d) local_world_prev_ hold check: after Host updates byte 0x01->0x02, Client's newer 0x03 arrives cleanly during hold
            host_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u, 0x01u);
            client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u, 0x01u);
            for (int t = 0; t < 22; ++t) {
                sync_step();
            }
            host_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u, 0x02u);
            for (int t = 0; t < 10; ++t) {
                sync_step();
                if (client_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u) == 0x02u) {
                    break;
                }
            }
            assert(client_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u) == 0x02u);
            client_bus.write8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u, 0x03u);
            for (int t = 0; t < 6; ++t) {
                sync_step();
                if (host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u) == 0x03u) {
                    break;
                }
            }
            assert(host_bus.read8(GS_ADDR + EwramCoopBridge::OFF_GS_COOP + 1u) == 0x03u);
        }
    }

    std::puts("test_ewram_coop: ALL PASSED");
    return 0;
}
