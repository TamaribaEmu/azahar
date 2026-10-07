// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Tamariba: Azahar's own multiplayer rooms (src/network: Network::Room and RoomMember, which
// carry the 3DS local wireless of NWM_UDS) for the libretro core. Tamariba runs every room on
// 127.0.0.1 and carries the packets between players through its Plaza relay (its "loopback
// relay"), so no player ever learns another's address:
//
//   host:  this console runs the room server on 127.0.0.1:<port> and joins it itself;
//   guest: this console joins 127.0.0.1:<port>, a port of Tamariba's relay that stands for the
//          host's room.
//
// The frontend reaches it through the azahar_room_* exports (citra_libretro.cpp, listed in
// azahar_libretro.map). Nothing here blocks the caller: joining (and trying again when the
// room is not there yet, or was lost) happens on a thread of its own.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common/common_types.h"
#include "network/room.h"
#include "network/room_member.h"

namespace Tamariba {

/// What azahar_room_state() returns.
enum RoomState : int {
    RoomIdle = 0,    ///< Not in a room
    RoomJoining = 1, ///< Connecting (or trying again)
    RoomJoined = 2,  ///< In the room: local wireless reaches the other consoles
    // Stopped trying (negative):
    RoomErrorConsoleId = -1, ///< Another console in the room has the same console ID
    RoomErrorVersion = -2,   ///< The room is another Azahar network version
    RoomErrorFull = -3,      ///< The room is full
    RoomErrorRefused = -4,   ///< Kicked, banned, wrong password or MAC address taken
    RoomErrorHost = -5,      ///< The room server could not start
};

/// A nickname Azahar's room accepts (^[ a-zA-Z0-9._-]{4,20}$) made from any name: other
/// characters are dropped, a short one is padded, a long one cut. `suffix` (> 1) is added as
/// " <n>" for a second try when the name is taken.
std::string RoomNickname(const std::string& name, int suffix = 1);

class RoomLink {
public:
    RoomLink(std::shared_ptr<Network::Room> room, std::shared_ptr<Network::RoomMember> member);
    ~RoomLink(); ///< Leaves (and waits for that)
    RoomLink(const RoomLink&) = delete;
    RoomLink& operator=(const RoomLink&) = delete;

    /// Starts the room server on 127.0.0.1:port (0: a free port) for at most `max_members`
    /// consoles and joins it. Returns the port, or 0 when the server could not start.
    u16 Host(u16 port, u32 max_members, const std::string& nickname, const Network::MacAddress& mac,
             const std::string& console_id_hash);
    /// Joins the room at address:port; keeps trying until it is in, or Leave().
    bool Join(const std::string& address, u16 port, const std::string& nickname,
              const Network::MacAddress& mac, const std::string& console_id_hash);
    /// Leaves the room (and closes it when hosting). Never waits: the leaving finishes on the
    /// worker thread; the next Host/Join (or the destructor) waits for it.
    void Leave();

    int State() const {
        return state.load();
    }
    /// The consoles in the room, by their nicknames (this one included).
    std::vector<std::string> Members() const;
    /// Datagrams the room server and this member send are at most this big (ENet's MTU).
    static constexpr u32 MaxDatagram = Network::MaxDatagramSize;

private:
    void Run(std::string address, u16 port, std::string nickname, Network::MacAddress mac,
             std::string console_id_hash);
    void Finish();    ///< Waits for a worker that is leaving (callers hold `mutex`)
    void CloseRoom(); ///< Hosting: closes the room (the worker, or Finish() after it)

    std::shared_ptr<Network::Room> room;
    std::shared_ptr<Network::RoomMember> member;
    std::mutex mutex; ///< Host/Join/Leave
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<int> state{RoomIdle};
    std::atomic<int> last_error{-1};
    std::atomic<bool> host_room{false}; ///< This console's room is open

    mutable std::mutex members_mutex;
    std::vector<std::string> members;
    Network::RoomMember::CallbackHandle<Network::RoomMember::Error> on_error;
    Network::RoomMember::CallbackHandle<Network::RoomInformation> on_information;
};

/// The console's RoomLink for the azahar_room_* exports (Network::Init's room and member).
/// Made by Init(), gone after Shutdown().
RoomLink* Rooms();
void InitRooms();
void ShutdownRooms();

/// A console ID hash for the room when no game runs (the room only compares them).
std::string FallbackConsoleIdHash(const Network::MacAddress& mac);

} // namespace Tamariba
