#pragma once

#include "game/fomt_domain.hpp"
#include "net_udp.hpp"

#include <array>
#include <functional>
#include <string>

namespace hm::net {

enum class NetRole : hal::u8 {
    Offline = 0,
    Host    = 1,
    Client  = 2,
};

enum class IntentKind : hal::u8 {
    UseTool    = 1,
    AdvanceDay = 2,
    SwitchMap  = 3,
};

enum class MsgType : hal::u8 {
    Hello        = 1,
    Welcome      = 2,
    TileChunk    = 3,
    PlayerUpdate = 4,
    ActionIntent = 5,
    PlayerStates = 6,
    TileDelta    = 7,
    AppPacket    = 16,
};

#pragma pack(push, 1)
struct PacketHeader {
    hal::u32 magic = 0x464F4D54u; // "FOMT"
    hal::u8  version = 1;
    MsgType  type = MsgType::Hello;
    hal::u8  player_id = 0;
    hal::u8  reserved = 0;
    hal::u32 seq = 0;
};
#pragma pack(pop)

class NetCoopSession {
public:
    static constexpr hal::u16 DEFAULT_PORT = 7777;
    static constexpr std::size_t TILES_PER_CHUNK = 128;

    NetCoopSession() = default;
    ~NetCoopSession();

    bool start_host(hal::u16 port = DEFAULT_PORT);
    bool connect_client(const std::string& host_port);
    void disconnect();

    NetRole role() const { return role_; }
    bool is_connected() const;
    hal::u8 local_player_id() const { return local_player_id_; }
    hal::u16 bound_port() const { return socket_.bound_port(); }
    std::size_t connected_peer_count() const;

    // Called every game tick (60 Hz) to pump incoming UDP packets and send periodic snapshots
    void tick(game::FarmWorld& world);

    // Client / Local triggers for authoritative actions
    void send_use_tool(game::FarmWorld& world, hal::i32 tile_x, hal::i32 tile_y, hal::u8 tool_id);
    void send_advance_day(game::FarmWorld& world);
    void send_switch_map(game::FarmWorld& world, hal::u16 map_id);

    // Optional app-level transport hook (e.g. EWRAM Coop Bridge in native ROM mode)
    using AppPacketHandler  = std::function<void(hal::u8 sender_id, const hal::u8* payload, std::size_t len)>;
    using PeerJoinedHandler = std::function<void(hal::u8 peer_player_id)>;

    void set_app_handlers(AppPacketHandler on_packet, PeerJoinedHandler on_peer_joined = {});
    void set_domain_sync_enabled(bool enabled) { domain_sync_enabled_ = enabled; }
    bool domain_sync_enabled() const { return domain_sync_enabled_; }
    bool send_app_to_host(const void* data, std::size_t len);
    bool send_app_to_peer(hal::u8 peer_id, const void* data, std::size_t len);
    void broadcast_app(const void* data, std::size_t len);

    std::string status_label() const;

private:
    struct PeerSlot {
        bool active = false;
        UdpEndpoint endpoint{};
        hal::u32 ticks_since_seen = 0;
    };

    void host_tick(game::FarmWorld& world);
    void client_tick(game::FarmWorld& world);

    void host_handle_packet(game::FarmWorld& world, const UdpEndpoint& src, const hal::u8* data, std::size_t len);
    void client_handle_packet(game::FarmWorld& world, const hal::u8* data, std::size_t len);

    void host_send_welcome_and_chunks(const game::FarmWorld& world, hal::u8 peer_player_id, const UdpEndpoint& dst);
    void host_broadcast_states(const game::FarmWorld& world);
    void host_broadcast_tile_delta(hal::u16 tx, hal::u16 ty, const game::FarmTile& tile, hal::u32 gold, hal::u8 actor_id);
    void host_broadcast_all_chunks(const game::FarmWorld& world);

    NetRole role_ = NetRole::Offline;
    UdpSocket socket_{};
    UdpEndpoint host_endpoint_{};
    bool client_welcomed_ = false;
    bool domain_sync_enabled_ = true;
    hal::u8 local_player_id_ = 0;
    hal::u32 seq_ = 0;
    hal::u32 tick_counter_ = 0;

    std::array<PeerSlot, game::MAX_COOP_PLAYERS> host_peers_{};
    AppPacketHandler on_app_packet_{};
    PeerJoinedHandler on_peer_joined_{};
};

} // namespace hm::net
