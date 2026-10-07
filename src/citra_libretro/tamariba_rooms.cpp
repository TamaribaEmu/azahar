// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cryptopp/sha.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "citra_libretro/tamariba_rooms.h"
#include "common/logging/log.h"
#include "network/network.h"

namespace Tamariba {

namespace {

using namespace std::chrono_literals;
using Network::RoomMember;

bool Allowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
           c == '.' || c == '_' || c == '-';
}

std::unique_ptr<RoomLink> g_rooms;

} // namespace

std::string RoomNickname(const std::string& name, int suffix) {
    std::string out;
    for (const char c : name) {
        if (Allowed(c)) {
            out.push_back(c);
        }
    }
    // No leading or trailing spaces, and none twice in a row.
    out.erase(
        std::unique(out.begin(), out.end(), [](char a, char b) { return a == ' ' && b == ' '; }),
        out.end());
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    const std::string tail = suffix > 1 ? fmt::format(" {}", suffix) : std::string();
    out.resize(std::min<std::size_t>(out.size(), 20 - tail.size()));
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "Player";
    }
    out += tail;
    while (out.size() < 4) {
        out.push_back('_');
    }
    return out;
}

std::string FallbackConsoleIdHash(const Network::MacAddress& mac) {
    std::array<u8, CryptoPP::SHA256::DIGESTSIZE> hash;
    CryptoPP::SHA256().CalculateDigest(hash.data(), mac.data(), mac.size());
    return fmt::format("{:02x}", fmt::join(hash.begin(), hash.end(), ""));
}

RoomLink::RoomLink(std::shared_ptr<Network::Room> room_,
                   std::shared_ptr<Network::RoomMember> member_)
    : room(std::move(room_)), member(std::move(member_)) {
    on_error = member->BindOnError(
        [this](const RoomMember::Error& error) { last_error = static_cast<int>(error); });
    // Called on the member's own thread right after it updated the list: a safe moment to copy it.
    on_information = member->BindOnRoomInformationChanged([this](const Network::RoomInformation&) {
        std::vector<std::string> names;
        for (const auto& info : member->GetMemberInformation()) {
            names.push_back(info.nickname);
        }
        std::lock_guard lock(members_mutex);
        members = std::move(names);
    });
}

RoomLink::~RoomLink() {
    {
        std::lock_guard lock(mutex);
        stop = true;
        Finish();
    }
    member->Unbind(on_error);
    member->Unbind(on_information);
}

void RoomLink::Finish() {
    if (worker.joinable()) {
        worker.join();
    }
    CloseRoom(); // the worker closed it, unless there was none
    std::lock_guard lock(members_mutex);
    members.clear();
}

u16 RoomLink::Host(u16 port, u32 max_members, const std::string& nickname,
                   const Network::MacAddress& mac, const std::string& console_id_hash) {
    std::lock_guard lock(mutex);
    stop = true;
    Finish();
    stop = false;
    // Only reachable from this device (127.0.0.1); the other players arrive through
    // Tamariba's relay. No password: the Plaza room decides who may play.
    if (!room->Create("Tamariba", "", "127.0.0.1", port, "", std::max<u32>(max_members, 1), "", "",
                      0, std::make_unique<Network::VerifyUser::NullBackend>(), {})) {
        LOG_ERROR(Network, "Tamariba: the room could not start on 127.0.0.1:{}", port);
        state = RoomErrorHost;
        return 0;
    }
    host_room = true;
    const u16 bound = room->GetRoomInformation().port;
    LOG_INFO(Network, "Tamariba: room on 127.0.0.1:{} for {} consoles", bound, max_members);
    state = RoomJoining;
    worker = std::thread(&RoomLink::Run, this, std::string("127.0.0.1"), bound,
                         RoomNickname(nickname), mac, console_id_hash);
    return bound;
}

bool RoomLink::Join(const std::string& address, u16 port, const std::string& nickname,
                    const Network::MacAddress& mac, const std::string& console_id_hash) {
    std::lock_guard lock(mutex);
    stop = true;
    Finish();
    stop = false;
    if (port == 0 || address.empty()) {
        state = RoomIdle;
        return false;
    }
    state = RoomJoining;
    worker = std::thread(&RoomLink::Run, this, address, port, RoomNickname(nickname), mac,
                         console_id_hash);
    return true;
}

void RoomLink::Leave() {
    std::lock_guard lock(mutex);
    stop = true;
    if (!worker.joinable()) {
        Finish();
    }
    // Otherwise the worker leaves by itself; Finish() runs with the next Host/Join or the end.
    state = RoomIdle;
}

std::vector<std::string> RoomLink::Members() const {
    std::lock_guard lock(members_mutex);
    return members;
}

void RoomLink::Run(std::string address, u16 port, std::string nickname, Network::MacAddress mac,
                   std::string console_id_hash) {
    const std::string base = nickname;
    int suffix = 1;
    // Waits up to `limit` while `busy` holds; false when told to stop.
    const auto wait_while = [this](auto busy, std::chrono::milliseconds limit) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (busy() && std::chrono::steady_clock::now() < end) {
            if (stop) {
                return false;
            }
            std::this_thread::sleep_for(10ms);
        }
        return !stop;
    };
    while (!stop) {
        last_error = -1;
        // Blocks until ENet connects or gives up (5 seconds).
        member->Join(nickname, console_id_hash, address.c_str(), port, 0, mac);
        // The room answers a join request at once (a round trip through the Plaza).
        if (!wait_while([this] { return member->GetState() == RoomMember::State::Joining; }, 4s)) {
            break;
        }
        const auto now = member->GetState();
        if (now == RoomMember::State::Joined || now == RoomMember::State::Moderator) {
            LOG_INFO(Network, "Tamariba: in the room as \"{}\"", nickname);
            state = RoomJoined;
            wait_while([this] { return member->IsConnected(); }, std::chrono::hours(24 * 365));
            if (stop) {
                break;
            }
            LOG_INFO(Network, "Tamariba: lost the room; trying again");
        }
        state = RoomJoining;
        member->Leave(); // a fresh ENet host for the next try
        switch (static_cast<RoomMember::Error>(last_error.load())) {
        case RoomMember::Error::NameCollision:
            nickname = RoomNickname(base, ++suffix);
            if (suffix > 8) {
                CloseRoom();
                state = RoomErrorRefused;
                return;
            }
            continue;
        case RoomMember::Error::ConsoleIdCollision:
            LOG_ERROR(Network, "Tamariba: another console in the room has this console's ID");
            CloseRoom();
            state = RoomErrorConsoleId;
            return;
        case RoomMember::Error::WrongVersion:
            CloseRoom();
            state = RoomErrorVersion;
            return;
        case RoomMember::Error::RoomIsFull:
            CloseRoom();
            state = RoomErrorFull;
            return;
        case RoomMember::Error::MacCollision:
        case RoomMember::Error::WrongPassword:
        case RoomMember::Error::HostKicked:
        case RoomMember::Error::HostBanned:
            CloseRoom();
            state = RoomErrorRefused;
            return;
        default:
            // Not there (yet), or lost: the host's game may still be starting.
            wait_while([] { return true; }, 1s);
            break;
        }
    }
    member->Leave(); // also frees the ENet host of a failed try
    CloseRoom();
    state = RoomIdle;
}

void RoomLink::CloseRoom() {
    // Hosting: the room closes as soon as this console leaves it (its members hear so).
    if (host_room && room->GetState() == Network::Room::State::Open) {
        room->Destroy();
    }
    host_room = false;
}

RoomLink* Rooms() {
    return g_rooms.get();
}

void InitRooms() {
    if (g_rooms) {
        return;
    }
    if (!Network::Init()) {
        LOG_ERROR(Network, "Tamariba: no networking (ENet did not start): no rooms");
        return;
    }
    g_rooms =
        std::make_unique<RoomLink>(Network::GetRoom().lock(), Network::GetRoomMember().lock());
}

void ShutdownRooms() {
    if (!g_rooms) {
        return;
    }
    g_rooms.reset();
    Network::Shutdown();
}

} // namespace Tamariba
