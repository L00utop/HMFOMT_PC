#include "game/fomt_domain.hpp"
#include "net/net_coop.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace hm::game;
using namespace hm::hal;
using namespace hm::net;

int main() {
    FarmWorld host_world;
    FarmWorld c1_world;
    FarmWorld c2_world;

    NetCoopSession host;
    NetCoopSession client1;
    NetCoopSession client2;

    assert(host.start_host(0)); // Ephemeral free localhost port
    const u16 port = host.bound_port();
    assert(port > 0);

    const std::string addr = "127.0.0.1:" + std::to_string(port);
    assert(client1.connect_client(addr));
    assert(client2.connect_client(addr));

    // Pump ticks until both clients finish handshake + chunk download
    for (int t = 0; t < 100 && (!client1.is_connected() || !client2.is_connected()); ++t) {
        host.tick(host_world);
        client1.tick(c1_world);
        client2.tick(c2_world);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(client1.is_connected());
    assert(client2.is_connected());
    assert(host.connected_peer_count() == 2);
    assert(client1.local_player_id() != client2.local_player_id());

    // Verify independent map support and position replication
    const u8 p1_id = client1.local_player_id();
    const u8 p2_id = client2.local_player_id();
    c1_world.farmers[p1_id].x = 16 * 14;
    c1_world.farmers[p1_id].y = 16 * 11;
    client2.send_switch_map(c2_world, 1);

    for (int t = 0; t < 15; ++t) {
        host.tick(host_world);
        client1.tick(c1_world);
        client2.tick(c2_world);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(host_world.farmers[p1_id].x == 16 * 14);
    assert(c2_world.farmers[p1_id].x == 16 * 14);
    assert(host_world.farmers[p2_id].map_id == 1);

    // Client 1 uses Hoe -> Turnip Seeds -> Watering Can at (14, 12)
    c1_world.tile_at(14, 12) = FarmTile{};
    host_world.tile_at(14, 12) = FarmTile{};

    client1.send_use_tool(c1_world, 14, 12, TOOL_IRON_HOE);
    for (int t = 0; t < 10; ++t) {
        host.tick(host_world);
        client1.tick(c1_world);
        client2.tick(c2_world);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(host_world.tile_at(14, 12).soil == SoilState::Tilled);
    assert(c1_world.tile_at(14, 12).soil == SoilState::Tilled);
    assert(c2_world.tile_at(14, 12).soil == SoilState::Tilled);

    client1.send_use_tool(c1_world, 14, 12, TOOL_TURNIP_SEEDS);
    client1.send_use_tool(c1_world, 14, 12, TOOL_WATERING_CAN);
    for (int t = 0; t < 15; ++t) {
        host.tick(host_world);
        client1.tick(c1_world);
        client2.tick(c2_world);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(host_world.tile_at(14, 12).crop == CropKind::Turnip);
    assert(host_world.tile_at(14, 12).soil == SoilState::Watered);
    assert(c2_world.tile_at(14, 12).crop == CropKind::Turnip);
    assert(c2_world.tile_at(14, 12).soil == SoilState::Watered);

    // Client 2 advances day -> all nodes advance & crop grows
    client2.send_advance_day(c2_world);
    for (int t = 0; t < 20; ++t) {
        host.tick(host_world);
        client1.tick(c1_world);
        client2.tick(c2_world);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(host_world.clock.day == 2);
    assert(c1_world.clock.day == 2);
    assert(c2_world.clock.day == 2);
    assert(host_world.tile_at(14, 12).growth_days == 1);
    assert(c1_world.tile_at(14, 12).growth_days == 1);
    assert(c2_world.tile_at(14, 12).growth_days == 1);

    std::puts("test_net_coop: ALL PASSED");
    return 0;
}
