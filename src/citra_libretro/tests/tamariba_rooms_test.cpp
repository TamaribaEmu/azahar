// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Tamariba: a check of tamariba_rooms.h without a game. Two consoles (two RoomLinks, each with
// its own Network::Room and RoomMember, as two devices would have) meet in one room on
// 127.0.0.1. The guest reaches the host's room through a UDP relay standing in for Tamariba's
// loopback relay, which notes the largest datagram it carried (it must fit the Plaza relay's
// 1200 bytes). Then local wireless frames (Network::WifiPacket, what NWM_UDS sends) go both ways.
//
//   cmake --build <build> --target tamariba_rooms_test && <build>/bin/Release/tamariba_rooms_test

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <enet/enet.h>
#include "citra_libretro/tamariba_rooms.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "libretro.h"

using namespace std::chrono_literals;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// A UDP relay on 127.0.0.1: what arrives on its port goes to the target port, and the answers
// go back to whoever sent last (one guest), like Tamariba's guest-side loopback relay.
class Relay {
public:
    explicit Relay(u16 target_port) : target(target_port) {
        socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        ENetAddress address{};
        enet_address_set_host(&address, "127.0.0.1");
        address.port = 0;
        enet_socket_bind(socket, &address);
        enet_socket_get_address(socket, &address);
        port = address.port;
        thread = std::thread([this] { Run(); });
    }
    ~Relay() {
        stop = true;
        thread.join();
        enet_socket_destroy(socket);
    }
    u16 port = 0;
    std::atomic<size_t> largest{0};
    std::atomic<u64> datagrams{0};

private:
    void Run() {
        ENetAddress guest{};
        bool have_guest = false;
        std::vector<u8> buffer(65536);
        while (!stop) {
            enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
            if (enet_socket_wait(socket, &wait, 10) < 0 || !(wait & ENET_SOCKET_WAIT_RECEIVE)) {
                continue;
            }
            ENetAddress from{};
            ENetBuffer data{buffer.data(), buffer.size()};
            const int n = enet_socket_receive(socket, &from, &data, 1);
            if (n <= 0) {
                continue;
            }
            largest = std::max(largest.load(), static_cast<size_t>(n));
            if (std::getenv("TAMARIBA_ROOMS_TRACE")) {
                std::fprintf(
                    stderr, "%lld relay %u -> %s (%d bytes)\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count() %
                                           100000),
                    from.port, from.port == target ? "guest" : "room", n);
            }
            ++datagrams;
            ENetBuffer out{buffer.data(), static_cast<size_t>(n)};
            if (from.port == target) {
                if (have_guest) {
                    enet_socket_send(socket, &guest, &out, 1);
                }
            } else {
                guest = from;
                have_guest = true;
                ENetAddress to{};
                enet_address_set_host(&to, "127.0.0.1");
                to.port = target;
                enet_socket_send(socket, &to, &out, 1);
            }
        }
    }
    ENetSocket socket;
    u16 target;
    std::atomic<bool> stop{false};
    std::thread thread;
};

struct Console {
    std::shared_ptr<Network::Room> room = std::make_shared<Network::Room>();
    std::shared_ptr<Network::RoomMember> member = std::make_shared<Network::RoomMember>();
    std::unique_ptr<Tamariba::RoomLink> link = std::make_unique<Tamariba::RoomLink>(room, member);
    std::mutex mutex;
    std::vector<Network::WifiPacket> received;
    Network::RoomMember::CallbackHandle<Network::WifiPacket> handle;
    Console() {
        handle = member->BindOnWifiPacketReceived([this](const Network::WifiPacket& p) {
            std::lock_guard lock(mutex);
            received.push_back(p);
        });
    }
    ~Console() {
        link.reset();
        member->Unbind(handle);
    }
    size_t Received() {
        std::lock_guard lock(mutex);
        return received.size();
    }
};

Network::MacAddress Mac(u8 last) {
    return {0x40, 0xF4, 0x07, 0x00, 0x00, last};
}

void TestNicknames() {
    using Tamariba::RoomNickname;
    CHECK(RoomNickname("Matt") == "Matt");
    CHECK(RoomNickname("Al") == "Al__");
    CHECK(RoomNickname("") == "Player");
    CHECK(RoomNickname("Zoë   Smith!") == "Zo Smith");
    CHECK(RoomNickname("Matt", 2) == "Matt 2");
    CHECK(RoomNickname("A very long display name indeed") == "A very long display");
    CHECK(RoomNickname("A very long display name indeed", 3) == "A very long displa 3");
    CHECK(RoomNickname("日本語") == "Player");
}

void TestTwoConsoles() {
    Console host, guest;
    // The guest asks first: the host's room is not there yet (a guest's game may start before
    // the host's). It keeps trying.
    const u16 port = host.link->Host(0, 4, "Matt", Mac(1), "aa");
    CHECK(port != 0);
    host.link->Leave();
    CHECK(host.link->State() == Tamariba::RoomIdle);
    CHECK(WaitFor([&] { return host.room->GetState() == Network::Room::State::Closed; }, 10s));
    std::this_thread::sleep_for(200ms); // its ENet host goes a moment after (Room::Destroy)
    Relay relay(port);
    CHECK(guest.link->Join("127.0.0.1", relay.port, "Matt", Mac(2), "bb"));
    std::this_thread::sleep_for(1500ms);
    CHECK(guest.link->State() == Tamariba::RoomJoining);

    const auto started = std::chrono::steady_clock::now();
    CHECK(host.link->Host(port, 4, "Matt", Mac(1), "aa") == port);
    CHECK(WaitFor([&] { return host.link->State() == Tamariba::RoomJoined; }, 5s));
    {
        int last = -100;
        WaitFor(
            [&] {
                const int now = guest.link->State();
                if (now != last) {
                    std::printf("  guest state %d at %lld ms (relay %llu datagrams)\n", now,
                                static_cast<long long>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()),
                                static_cast<unsigned long long>(relay.datagrams.load()));
                    last = now;
                }
                return now == Tamariba::RoomJoined;
            },
            15s);
    }
    CHECK(guest.link->State() == Tamariba::RoomJoined);
    const auto joined_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    std::printf("guest joined %lld ms after the host's room started\n",
                static_cast<long long>(joined_ms));
    // Same display name: the second one in gets " 2".
    CHECK(WaitFor([&] { return guest.link->Members().size() == 2; }, 5s));
    CHECK(WaitFor([&] { return host.link->Members().size() == 2; }, 5s));
    const auto names = host.link->Members();
    CHECK(std::find(names.begin(), names.end(), "Matt") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Matt 2") != names.end());

    // Local wireless frames both ways, one larger than a datagram (ENet splits it).
    Network::WifiPacket beacon{};
    beacon.type = Network::WifiPacket::PacketType::Beacon;
    beacon.channel = 1;
    beacon.destination_address = Network::BroadcastMac;
    beacon.data.resize(1400);
    for (size_t i = 0; i < beacon.data.size(); ++i) {
        beacon.data[i] = static_cast<u8>(i * 7);
    }
    beacon.transmitter_address = host.member->GetMacAddress();
    CHECK(host.member->GetMacAddress() == Mac(1));
    CHECK(guest.member->GetMacAddress() == Mac(2));
    const auto sent = std::chrono::steady_clock::now();
    host.member->SendWifiPacket(beacon);
    CHECK(WaitFor([&] { return guest.Received() >= 1; }, 5s));
    std::printf("1400-byte frame host -> guest: %lld us\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - sent)
                                           .count()));
    {
        std::lock_guard lock(guest.mutex);
        CHECK(!guest.received.empty() && guest.received[0].data == beacon.data);
        CHECK(!guest.received.empty() && guest.received[0].transmitter_address == Mac(1));
    }
    Network::WifiPacket data{};
    data.type = Network::WifiPacket::PacketType::Data;
    data.channel = 1;
    data.transmitter_address = Mac(2);
    data.destination_address = Mac(1);
    data.data = {1, 2, 3, 4, 5};
    guest.member->SendWifiPacket(data);
    CHECK(WaitFor([&] { return host.Received() >= 1; }, 5s));
    {
        std::lock_guard lock(host.mutex);
        CHECK(!host.received.empty() && host.received[0].data == data.data);
    }
    std::printf("relay: %llu datagrams, the largest %zu bytes\n",
                static_cast<unsigned long long>(relay.datagrams.load()), relay.largest.load());
    CHECK(relay.largest.load() <= 1200);
    CHECK(relay.largest.load() > 1000); // the big frame was split into full datagrams

    // The guest leaves; the host sees it go. Then it comes back.
    guest.link->Leave();
    CHECK(WaitFor([&] { return host.link->Members().size() == 1; }, 10s));
    CHECK(guest.link->Join("127.0.0.1", relay.port, "Sam", Mac(2), "bb"));
    CHECK(WaitFor([&] { return guest.link->State() == Tamariba::RoomJoined; }, 15s));

    // Another console with the same console ID is turned away, and says why.
    Console twin;
    CHECK(twin.link->Join("127.0.0.1", port, "Twin", Mac(3), "bb"));
    CHECK(WaitFor([&] { return twin.link->State() == Tamariba::RoomErrorConsoleId; }, 10s));

    // The host leaves: the room closes, the guest goes back to trying.
    host.link->Leave();
    CHECK(WaitFor([&] { return guest.link->State() == Tamariba::RoomJoining; }, 15s));
}

} // namespace

void PrintLog(enum retro_log_level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

int main() {
    // TAMARIBA_ROOMS_LOG=1: Azahar's own log (the network's included) on stderr.
    static Common::Log::Filter filter(Common::Log::Level::Debug);
    if (const char* log = std::getenv("TAMARIBA_ROOMS_LOG"); log && log[0] == '1') {
        Common::Log::LibRetroStart(PrintLog);
        Common::Log::SetGlobalFilter(filter);
    }
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "ENet did not start\n");
        return 1;
    }
    TestNicknames();
    TestTwoConsoles();
    enet_deinitialize();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("tamariba_rooms_test: all passed\n");
    return 0;
}
