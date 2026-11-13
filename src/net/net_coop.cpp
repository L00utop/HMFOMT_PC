#include "net_coop.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hm::net {

using namespace hm::hal;
using namespace hm::game;

namespace {

#pragma pack(push, 1)
struct WelcomePayload {
    PacketHeader hdr;
    u8 assigned_player_id;
    WorldClock clock;
    FarmerState farmers[MAX_COOP_PLAYERS];
};

struct TileChunkPayload {
    PacketHeader hdr;
    u16 start_index;
    u16 count;
    FarmTile tiles[NetCoopSession::TILES_PER_CHUNK];
};

struct PlayerUpdatePayload {
    PacketHeader hdr;
    u16 map_id;
    i16 x;
    i16 y;
    Facing facing;
    u8 anim_frame;
    u8 tool_anim_timer;
    u8 equipped_tool;
};

struct ActionIntentPayload {
    PacketHeader hdr;
    IntentKind kind;
    u8 tool_id;
    i16 tile_x;
    i16 tile_y;
    u16 target_map;
};

struct PlayerStatesPayload {
    PacketHeader hdr;
    WorldClock clock;
    FarmerState farmers[MAX_COOP_PLAYERS];
};

struct TileDeltaPayload {
    PacketHeader hdr;
    u16 tx;
    u16 ty;
    FarmTile tile;
    u8 actor_id;
    u32 actor_gold;
};
#pragma pack(pop)

} // namespace

NetCoopSession::~NetCoopSession() {
    disconnect();
}

bool NetCoopSession::start_host(u16 port) {
    disconnect();
    if (!socket_.open(port)) return false;
    role_ = NetRole::Host;
    local_player_id_ = 0;
    client_welcomed_ = true;
    tick_counter_ = 0;
    seq_ = 0;
    for (auto& p : host_peers_) p = PeerSlot{};
    return true;
}

bool NetCoopSession::connect_client(const std::string& host_port) {
    disconnect();
    if (!UdpEndpoint::parse(host_port, DEFAULT_PORT, host_endpoint_)) return false;
    if (!socket_.open(0)) return false;
    role_ = NetRole::Client;
    local_player_id_ = 0;
    client_welcomed_ = false;
    tick_counter_ = 0;
    seq_ = 0;

    PacketHeader hello{};
    hello.type = MsgType::Hello;
    hello.seq = ++seq_;
    socket_.send_to(host_endpoint_, &hello, sizeof(hello));
    return true;
}

void NetCoopSession::disconnect() {
    socket_.close();
    role_ = NetRole::Offline;
    client_welcomed_ = false;
    local_player_id_ = 0;
    for (auto& p : host_peers_) p = PeerSlot{};
}

bool NetCoopSession::is_connected() const {
    if (role_ == NetRole::Host) return socket_.is_open();
    if (role_ == NetRole::Client) return socket_.is_open() && client_welcomed_;
    return false;
}

std::size_t NetCoopSession::connected_peer_count() const {
    if (role_ == NetRole::Host) {
        std::size_t c = 0;
        for (std::size_t i = 1; i < MAX_COOP_PLAYERS; ++i) {
            if (host_peers_[i].active) ++c;
        }
        return c;
    }
    return client_welcomed_ ? 1u : 0u;
}

std::string NetCoopSession::status_label() const {
    if (role_ == NetRole::Host) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "HOST:%u P%zu", socket_.bound_port(), connected_peer_count() + 1);
        return buf;
    }
    if (role_ == NetRole::Client) {
        if (!client_welcomed_) return "CLIENT:WAIT";
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "COOP:P%u", static_cast<unsigned>(local_player_id_ + 1));
        return buf;
    }
    return "LOCAL";
}

void NetCoopSession::set_app_handlers(AppPacketHandler on_packet, PeerJoinedHandler on_peer_joined) {
    on_app_packet_ = std::move(on_packet);
    on_peer_joined_ = std::move(on_peer_joined);
}

bool NetCoopSession::send_app_to_host(const void* data, std::size_t len) {
    if (role_ != NetRole::Client || !client_welcomed_ || !socket_.is_open() || !data || len == 0 || len > 1300u) {
        return false;
    }
    std::array<u8, 1360> buf{};
    PacketHeader hdr{};
    hdr.type = MsgType::AppPacket;
    hdr.player_id = local_player_id_;
    hdr.seq = ++seq_;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), data, len);
    return socket_.send_to(host_endpoint_, buf.data(), sizeof(hdr) + len);
}

bool NetCoopSession::send_app_to_peer(u8 peer_id, const void* data, std::size_t len) {
    if (role_ != NetRole::Host || !socket_.is_open() || peer_id == 0 || peer_id >= MAX_COOP_PLAYERS ||
        !host_peers_[peer_id].active || !data || len == 0 || len > 1300u) {
        return false;
    }
    std::array<u8, 1360> buf{};
    PacketHeader hdr{};
    hdr.type = MsgType::AppPacket;
    hdr.player_id = local_player_id_;
    hdr.seq = ++seq_;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), data, len);
    return socket_.send_to(host_peers_[peer_id].endpoint, buf.data(), sizeof(hdr) + len);
}

void NetCoopSession::broadcast_app(const void* data, std::size_t len) {
    if (role_ == NetRole::Client) {
        send_app_to_host(data, len);
        return;
    }
    if (role_ == NetRole::Host) {
        for (u8 i = 1; i < static_cast<u8>(MAX_COOP_PLAYERS); ++i) {
            if (host_peers_[i].active) {
                send_app_to_peer(i, data, len);
            }
        }
    }
}

void NetCoopSession::tick(FarmWorld& world) {
    ++tick_counter_;
    if (role_ == NetRole::Host) {
        host_tick(world);
    } else if (role_ == NetRole::Client) {
        client_tick(world);
    }
}

void NetCoopSession::host_tick(FarmWorld& world) {
    u8 buf[1400]{};
    UdpEndpoint src{};
    for (int i = 0; i < 32; ++i) {
        const int n = socket_.recv_from(src, buf, sizeof(buf));
        if (n <= 0) break;
        host_handle_packet(world, src, buf, static_cast<std::size_t>(n));
    }

    for (std::size_t i = 1; i < MAX_COOP_PLAYERS; ++i) {
        if (host_peers_[i].active) {
            if (++host_peers_[i].ticks_since_seen > 600u) {
                host_peers_[i].active = false;
                world.farmers[i].connected = false;
            }
        }
    }

    // Broadcast authoritative clock + player positions every 2 ticks (~30 Hz)
    if (domain_sync_enabled_ && (tick_counter_ % 2u) == 0u) {
        host_broadcast_states(world);
    }
}

void NetCoopSession::client_tick(FarmWorld& world) {
    u8 buf[1400]{};
    UdpEndpoint src{};
    for (int i = 0; i < 64; ++i) {
        const int n = socket_.recv_from(src, buf, sizeof(buf));
        if (n <= 0) break;
        client_handle_packet(world, buf, static_cast<std::size_t>(n));
    }

    if (!client_welcomed_) {
        if ((tick_counter_ % 15u) == 0u) {
            PacketHeader hello{};
            hello.type = MsgType::Hello;
            hello.seq = ++seq_;
            socket_.send_to(host_endpoint_, &hello, sizeof(hello));
        }
        return;
    }

    if (domain_sync_enabled_ && (tick_counter_ % 2u) == 0u) {
        const auto& me = world.farmers[local_player_id_];
        PlayerUpdatePayload up{};
        up.hdr.type = MsgType::PlayerUpdate;
        up.hdr.player_id = local_player_id_;
        up.hdr.seq = ++seq_;
        up.map_id = me.map_id;
        up.x = me.x;
        up.y = me.y;
        up.facing = me.facing;
        up.anim_frame = me.anim_frame;
        up.tool_anim_timer = me.tool_anim_timer;
        up.equipped_tool = me.equipped_tool;
        socket_.send_to(host_endpoint_, &up, sizeof(up));
    }
}

void NetCoopSession::host_handle_packet(FarmWorld& world, const UdpEndpoint& src, const u8* data, std::size_t len) {
    if (len < sizeof(PacketHeader)) return;
    PacketHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != 0x464F4D54u || hdr.version != 1) return;

    if (hdr.type == MsgType::Hello) {
        int slot = -1;
        for (int i = 1; i < static_cast<int>(MAX_COOP_PLAYERS); ++i) {
            if (host_peers_[i].active && host_peers_[i].endpoint == src) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            for (int i = 1; i < static_cast<int>(MAX_COOP_PLAYERS); ++i) {
                if (!host_peers_[i].active) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot > 0) {
            host_peers_[slot].active = true;
            host_peers_[slot].endpoint = src;
            host_peers_[slot].ticks_since_seen = 0;
            world.farmers[slot].id = static_cast<u8>(slot);
            world.farmers[slot].connected = true;
            host_send_welcome_and_chunks(world, static_cast<u8>(slot), src);
            if (on_peer_joined_) {
                on_peer_joined_(static_cast<u8>(slot));
            }
        }
        return;
    }

    const u8 pid = hdr.player_id;
    if (pid == 0 || pid >= MAX_COOP_PLAYERS || !host_peers_[pid].active) return;
    host_peers_[pid].ticks_since_seen = 0;

    if (hdr.type == MsgType::AppPacket) {
        if (on_app_packet_ && len > sizeof(PacketHeader)) {
            on_app_packet_(pid, data + sizeof(PacketHeader), len - sizeof(PacketHeader));
        }
        return;
    }

    if (hdr.type == MsgType::PlayerUpdate && len >= sizeof(PlayerUpdatePayload)) {
        PlayerUpdatePayload up{};
        std::memcpy(&up, data, sizeof(up));
        auto& f = world.farmers[pid];
        f.connected = true;
        f.map_id = up.map_id;
        f.x = std::clamp<i16>(up.x, 0, 2048);
        f.y = std::clamp<i16>(up.y, 0, 2048);
        f.facing = up.facing;
        f.anim_frame = up.anim_frame;
        f.tool_anim_timer = up.tool_anim_timer;
        f.equipped_tool = up.equipped_tool;
    } else if (hdr.type == MsgType::ActionIntent && len >= sizeof(ActionIntentPayload)) {
        ActionIntentPayload intent{};
        std::memcpy(&intent, data, sizeof(intent));
        auto& f = world.farmers[pid];

        if (intent.kind == IntentKind::UseTool) {
            // Distance sanity check (authoritative validation)
            const i32 fx = f.x / 16;
            const i32 fy = f.y / 16;
            if (std::abs(fx - intent.tile_x) <= 2 && std::abs(fy - intent.tile_y) <= 2 &&
                world.in_bounds(intent.tile_x, intent.tile_y)) {
                const auto res = world.apply_tool(f, intent.tile_x, intent.tile_y, intent.tool_id);
                if (res.modified_tile) {
                    host_broadcast_tile_delta(
                        static_cast<u16>(intent.tile_x),
                        static_cast<u16>(intent.tile_y),
                        world.tile_at(static_cast<u32>(intent.tile_x), static_cast<u32>(intent.tile_y)),
                        f.gold,
                        pid);
                }
            }
        } else if (intent.kind == IntentKind::AdvanceDay) {
            world.advance_day();
            host_broadcast_all_chunks(world);
            host_broadcast_states(world);
        } else if (intent.kind == IntentKind::SwitchMap) {
            f.map_id = intent.target_map;
        }
    }
}

void NetCoopSession::client_handle_packet(FarmWorld& world, const u8* data, std::size_t len) {
    if (len < sizeof(PacketHeader)) return;
    PacketHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != 0x464F4D54u || hdr.version != 1) return;

    if (hdr.type == MsgType::Welcome && len >= sizeof(WelcomePayload)) {
        WelcomePayload wp{};
        std::memcpy(&wp, data, sizeof(wp));
        if (wp.assigned_player_id < MAX_COOP_PLAYERS) {
            local_player_id_ = wp.assigned_player_id;
            client_welcomed_ = true;
            world.clock = wp.clock;
            for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
                world.farmers[i] = wp.farmers[i];
            }
        }
    } else if (hdr.type == MsgType::TileChunk && len >= sizeof(TileChunkPayload)) {
        TileChunkPayload cp{};
        std::memcpy(&cp, data, sizeof(cp));
        for (u16 i = 0; i < cp.count && i < TILES_PER_CHUNK; ++i) {
            const std::size_t idx = static_cast<std::size_t>(cp.start_index) + i;
            if (idx < FARM_TILE_COUNT) {
                world.tiles()[idx] = cp.tiles[i];
            }
        }
    } else if (hdr.type == MsgType::PlayerStates && len >= sizeof(PlayerStatesPayload)) {
        PlayerStatesPayload ps{};
        std::memcpy(&ps, data, sizeof(ps));
        world.clock = ps.clock;
        for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
            if (i == local_player_id_) {
                // Keep responsive local movement, adopt authoritative gold & stamina
                world.farmers[i].gold = ps.farmers[i].gold;
                world.farmers[i].stamina = ps.farmers[i].stamina;
                world.farmers[i].connected = true;
            } else {
                world.farmers[i] = ps.farmers[i];
            }
        }
    } else if (hdr.type == MsgType::AppPacket) {
        if (on_app_packet_ && len > sizeof(PacketHeader)) {
            on_app_packet_(hdr.player_id, data + sizeof(PacketHeader), len - sizeof(PacketHeader));
        }
    } else if (hdr.type == MsgType::TileDelta && len >= sizeof(TileDeltaPayload)) {
        TileDeltaPayload td{};
        std::memcpy(&td, data, sizeof(td));
        if (world.in_bounds(td.tx, td.ty)) {
            world.tile_at(td.tx, td.ty) = td.tile;
        }
        if (td.actor_id < MAX_COOP_PLAYERS) {
            world.farmers[td.actor_id].gold = td.actor_gold;
        }
    }
}

void NetCoopSession::host_send_welcome_and_chunks(const FarmWorld& world, u8 peer_player_id, const UdpEndpoint& dst) {
    WelcomePayload wp{};
    wp.hdr.type = MsgType::Welcome;
    wp.hdr.seq = ++seq_;
    wp.assigned_player_id = peer_player_id;
    wp.clock = world.clock;
    for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
        wp.farmers[i] = world.farmers[i];
    }
    socket_.send_to(dst, &wp, sizeof(wp));

    for (std::size_t start = 0; start < FARM_TILE_COUNT; start += TILES_PER_CHUNK) {
        TileChunkPayload cp{};
        cp.hdr.type = MsgType::TileChunk;
        cp.hdr.seq = ++seq_;
        cp.start_index = static_cast<u16>(start);
        cp.count = static_cast<u16>(std::min(TILES_PER_CHUNK, FARM_TILE_COUNT - start));
        for (u16 i = 0; i < cp.count; ++i) {
            cp.tiles[i] = world.tiles()[start + i];
        }
        socket_.send_to(dst, &cp, sizeof(cp));
    }
}

void NetCoopSession::host_broadcast_states(const FarmWorld& world) {
    PlayerStatesPayload ps{};
    ps.hdr.type = MsgType::PlayerStates;
    ps.hdr.seq = ++seq_;
    ps.clock = world.clock;
    for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
        ps.farmers[i] = world.farmers[i];
    }
    for (std::size_t i = 1; i < MAX_COOP_PLAYERS; ++i) {
        if (host_peers_[i].active) {
            socket_.send_to(host_peers_[i].endpoint, &ps, sizeof(ps));
        }
    }
}

void NetCoopSession::host_broadcast_tile_delta(u16 tx, u16 ty, const FarmTile& tile, u32 gold, u8 actor_id) {
    TileDeltaPayload td{};
    td.hdr.type = MsgType::TileDelta;
    td.hdr.seq = ++seq_;
    td.tx = tx;
    td.ty = ty;
    td.tile = tile;
    td.actor_id = actor_id;
    td.actor_gold = gold;
    for (std::size_t i = 1; i < MAX_COOP_PLAYERS; ++i) {
        if (host_peers_[i].active) {
            socket_.send_to(host_peers_[i].endpoint, &td, sizeof(td));
        }
    }
}

void NetCoopSession::host_broadcast_all_chunks(const FarmWorld& world) {
    for (std::size_t start = 0; start < FARM_TILE_COUNT; start += TILES_PER_CHUNK) {
        TileChunkPayload cp{};
        cp.hdr.type = MsgType::TileChunk;
        cp.hdr.seq = ++seq_;
        cp.start_index = static_cast<u16>(start);
        cp.count = static_cast<u16>(std::min(TILES_PER_CHUNK, FARM_TILE_COUNT - start));
        for (u16 i = 0; i < cp.count; ++i) {
            cp.tiles[i] = world.tiles()[start + i];
        }
        for (std::size_t p = 1; p < MAX_COOP_PLAYERS; ++p) {
            if (host_peers_[p].active) {
                socket_.send_to(host_peers_[p].endpoint, &cp, sizeof(cp));
            }
        }
    }
}

void NetCoopSession::send_use_tool(FarmWorld& world, i32 tile_x, i32 tile_y, u8 tool_id) {
    if (role_ == NetRole::Client && client_welcomed_) {
        ActionIntentPayload intent{};
        intent.hdr.type = MsgType::ActionIntent;
        intent.hdr.player_id = local_player_id_;
        intent.hdr.seq = ++seq_;
        intent.kind = IntentKind::UseTool;
        intent.tool_id = tool_id;
        intent.tile_x = static_cast<i16>(tile_x);
        intent.tile_y = static_cast<i16>(tile_y);
        socket_.send_to(host_endpoint_, &intent, sizeof(intent));
        return;
    }

    auto& f = world.farmers[local_player_id_];
    const auto res = world.apply_tool(f, tile_x, tile_y, tool_id);
    if (role_ == NetRole::Host && res.modified_tile && world.in_bounds(tile_x, tile_y)) {
        host_broadcast_tile_delta(
            static_cast<u16>(tile_x),
            static_cast<u16>(tile_y),
            world.tile_at(static_cast<u32>(tile_x), static_cast<u32>(tile_y)),
            f.gold,
            local_player_id_);
    }
}

void NetCoopSession::send_advance_day(FarmWorld& world) {
    if (role_ == NetRole::Client && client_welcomed_) {
        ActionIntentPayload intent{};
        intent.hdr.type = MsgType::ActionIntent;
        intent.hdr.player_id = local_player_id_;
        intent.hdr.seq = ++seq_;
        intent.kind = IntentKind::AdvanceDay;
        socket_.send_to(host_endpoint_, &intent, sizeof(intent));
        return;
    }

    world.advance_day();
    if (role_ == NetRole::Host) {
        host_broadcast_all_chunks(world);
        host_broadcast_states(world);
    }
}

void NetCoopSession::send_switch_map(FarmWorld& world, u16 map_id) {
    world.farmers[local_player_id_].map_id = map_id;
    if (role_ == NetRole::Client && client_welcomed_) {
        ActionIntentPayload intent{};
        intent.hdr.type = MsgType::ActionIntent;
        intent.hdr.player_id = local_player_id_;
        intent.hdr.seq = ++seq_;
        intent.kind = IntentKind::SwitchMap;
        intent.target_map = map_id;
        socket_.send_to(host_endpoint_, &intent, sizeof(intent));
    }
}

} // namespace hm::net
