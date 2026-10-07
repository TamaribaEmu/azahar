// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Tamariba: a check of core/tamariba_inet.h, the console's sockets (soc:U) and HTTP (http:C)
// through Tamariba's internet interface. A stand-in for the Plaza's exit carries everything on
// this machine's loopback (its lookups know one name, "pretendo.test"), and counts what went
// through it. Linux only (the stand-in uses POSIX sockets).
//
//   cmake --build <build> --target tamariba_inet_test && <build>/bin/Release/tamariba_inet_test

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <httplib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "core/tamariba_inet.h"

using namespace std::chrono_literals;
namespace VNet = Tamariba::VNet;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// The stand-in exit: non-blocking host sockets on 127.0.0.1.
struct FakeExit {
    std::mutex mutex;
    std::map<int, int> fds; // handle -> host socket
    std::map<int, u32> lookups;
    int next = 1;
    std::atomic<int> tcp_connects{0}, udp_datagrams{0}, lookups_made{0};
    tamariba_inet_interface iface{};

    FakeExit() {
        iface.version = TAMARIBA_NET_VERSION;
        iface.ctx = this;
        iface.public_ipv4 = [](void*) -> uint32_t { return htonl(0xCB007105); };
        iface.resolve = [](void* c, const char* host) -> int {
            auto& self = *static_cast<FakeExit*>(c);
            std::lock_guard lock(self.mutex);
            ++self.lookups_made;
            const int h = self.next++;
            self.lookups[h] = std::string(host) == "pretendo.test" ? htonl(INADDR_LOOPBACK) : 0;
            return h;
        };
        iface.resolve_result = [](void* c, int h, uint32_t* ip) -> int {
            auto& self = *static_cast<FakeExit*>(c);
            std::lock_guard lock(self.mutex);
            const auto it = self.lookups.find(h);
            if (it == self.lookups.end()) {
                return TAMARIBA_INET_ERROR;
            }
            const u32 found = it->second;
            self.lookups.erase(it);
            if (!found) {
                return TAMARIBA_INET_REFUSED;
            }
            *ip = found;
            return TAMARIBA_INET_OK;
        };
        iface.tcp_connect = [](void* c, uint32_t ip, uint16_t port) -> int {
            auto& self = *static_cast<FakeExit*>(c);
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            fcntl(fd, F_SETFL, O_NONBLOCK);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = ip;
            a.sin_port = htons(port);
            ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            ++self.tcp_connects;
            std::lock_guard lock(self.mutex);
            self.fds[self.next] = fd;
            return self.next++;
        };
        iface.tcp_state = [](void* c, int h) -> int {
            const int fd = static_cast<FakeExit*>(c)->Fd(h);
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, 0) <= 0) {
                return TAMARIBA_INET_AGAIN;
            }
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            return error == 0 ? TAMARIBA_INET_OK : TAMARIBA_INET_REFUSED;
        };
        iface.tcp_send = [](void* c, int h, const void* data, size_t size) -> int {
            const auto n = ::send(static_cast<FakeExit*>(c)->Fd(h), data,
                                  std::min<size_t>(size, 1000), MSG_NOSIGNAL);
            return n >= 0 ? static_cast<int>(n)
                          : (errno == EAGAIN ? TAMARIBA_INET_AGAIN : TAMARIBA_INET_CLOSED);
        };
        iface.tcp_recv = [](void* c, int h, void* buf, size_t cap) -> int {
            const auto n = ::recv(static_cast<FakeExit*>(c)->Fd(h), buf, cap, 0);
            if (n > 0) {
                return static_cast<int>(n);
            }
            return n == 0 ? TAMARIBA_INET_CLOSED
                          : (errno == EAGAIN ? TAMARIBA_INET_AGAIN : TAMARIBA_INET_CLOSED);
        };
        iface.tcp_close = [](void* c, int h) { static_cast<FakeExit*>(c)->Close(h); };
        iface.udp_open = [](void* c, uint16_t) -> int {
            auto& self = *static_cast<FakeExit*>(c);
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            fcntl(fd, F_SETFL, O_NONBLOCK);
            std::lock_guard lock(self.mutex);
            self.fds[self.next] = fd;
            return self.next++;
        };
        iface.udp_sendto = [](void* c, int h, uint32_t ip, uint16_t port, const void* data,
                              size_t size) -> int {
            auto& self = *static_cast<FakeExit*>(c);
            if (ntohl(ip) >> 24 == 10) {
                return TAMARIBA_INET_REFUSED; // private: the Plaza's rules
            }
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = ip;
            a.sin_port = htons(port);
            ++self.udp_datagrams;
            const auto n =
                ::sendto(self.Fd(h), data, size, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            return n >= 0 ? static_cast<int>(n) : TAMARIBA_INET_ERROR;
        };
        iface.udp_recvfrom = [](void* c, int h, uint32_t* ip, uint16_t* port, void* buf,
                                size_t cap) -> int {
            sockaddr_in a{};
            socklen_t len = sizeof(a);
            const auto n = ::recvfrom(static_cast<FakeExit*>(c)->Fd(h), buf, cap, 0,
                                      reinterpret_cast<sockaddr*>(&a), &len);
            if (n < 0) {
                return TAMARIBA_INET_AGAIN;
            }
            *ip = a.sin_addr.s_addr;
            *port = ntohs(a.sin_port);
            return static_cast<int>(n);
        };
        iface.udp_close = [](void* c, int h) { static_cast<FakeExit*>(c)->Close(h); };
    }
    int Fd(int h) {
        std::lock_guard lock(mutex);
        const auto it = fds.find(h);
        return it == fds.end() ? -1 : it->second;
    }
    void Close(int h) {
        std::lock_guard lock(mutex);
        if (const auto it = fds.find(h); it != fds.end()) {
            ::close(it->second);
            fds.erase(it);
        }
    }
};

// An echo server on 127.0.0.1 (UDP and TCP on one port number each).
struct Echo {
    int udp = -1, tcp = -1;
    u16 udp_port = 0, tcp_port = 0;
    std::atomic<bool> stop{false};
    std::thread thread;
    Echo() {
        udp = ::socket(AF_INET, SOCK_DGRAM, 0);
        tcp = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        ::bind(udp, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::getsockname(udp, reinterpret_cast<sockaddr*>(&a), &len);
        udp_port = ntohs(a.sin_port);
        a.sin_port = 0;
        ::bind(tcp, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(tcp, 4);
        ::getsockname(tcp, reinterpret_cast<sockaddr*>(&a), &len);
        tcp_port = ntohs(a.sin_port);
        thread = std::thread([this] { Run(); });
    }
    ~Echo() {
        stop = true;
        thread.join();
        ::close(udp);
        ::close(tcp);
    }
    void Run() {
        std::vector<int> clients;
        char buffer[4096];
        while (!stop) {
            std::vector<pollfd> p{{udp, POLLIN, 0}, {tcp, POLLIN, 0}};
            for (int c : clients) {
                p.push_back({c, POLLIN, 0});
            }
            if (::poll(p.data(), p.size(), 10) <= 0) {
                continue;
            }
            if (p[0].revents) {
                sockaddr_in from{};
                socklen_t len = sizeof(from);
                const auto n = ::recvfrom(udp, buffer, sizeof(buffer), 0,
                                          reinterpret_cast<sockaddr*>(&from), &len);
                if (n > 0) {
                    ::sendto(udp, buffer, n, 0, reinterpret_cast<sockaddr*>(&from), len);
                }
            }
            if (p[1].revents) {
                clients.push_back(::accept(tcp, nullptr, nullptr));
            }
            for (size_t i = 2; i < p.size(); ++i) {
                if (!p[i].revents) {
                    continue;
                }
                const auto n = ::recv(p[i].fd, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    ::close(p[i].fd);
                    clients.erase(std::find(clients.begin(), clients.end(), p[i].fd));
                    break;
                }
                ::send(p[i].fd, buffer, n, MSG_NOSIGNAL);
            }
        }
        for (int c : clients) {
            ::close(c);
        }
    }
};

sockaddr_in Loopback(u16 port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

void TestNoNetwork() {
    Tamariba::Inet::SetInterface(nullptr);
    CHECK(VNet::socket(AF_INET, SOCK_DGRAM, 0) == -1 && VNet::LastError() == ENETDOWN);
    CHECK(VNet::gethostbyname("pretendo.test") == nullptr);
    addrinfo* res = nullptr;
    CHECK(VNet::getaddrinfo("pretendo.test", "80", nullptr, &res) != 0 && res == nullptr);
    CHECK(Tamariba::Inet::HttpProxyPort() == 0);
    CHECK(!Tamariba::Inet::GetInterfaceInfo());
}

void TestSockets(FakeExit& exit, Echo& echo) {
    Tamariba::Inet::SetInterface(&exit.iface);
    // Names: through the exit's lookups.
    const hostent* h = VNet::gethostbyname("pretendo.test");
    CHECK(h && h->h_length == 4 &&
          *reinterpret_cast<const u32*>(h->h_addr_list[0]) == htonl(INADDR_LOOPBACK));
    CHECK(VNet::gethostbyname("nintendo.test") == nullptr);
    addrinfo* res = nullptr;
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    CHECK(VNet::getaddrinfo("pretendo.test", "1234", &hints, &res) == 0 && res && !res->ai_next);
    CHECK(res && reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_port == htons(1234));
    VNet::freeaddrinfo(res);

    // UDP: a datagram to the echo server and back.
    const int u = VNet::socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u > 0);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(60001);
    CHECK(VNet::bind(u, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0);
    sockaddr_in name{};
    socklen_t name_len = sizeof(name);
    CHECK(VNet::getsockname(u, reinterpret_cast<sockaddr*>(&name), &name_len) == 0 &&
          ntohs(name.sin_port) == 60001);
    auto to = Loopback(echo.udp_port);
    CHECK(VNet::sendto(u, "hello", 5, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 5);
    pollfd p{u, POLLIN, 0};
    CHECK(VNet::poll(&p, 1, 2000) == 1 && (p.revents & POLLIN));
    char buffer[64] = {};
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    CHECK(VNet::recvfrom(u, buffer, sizeof(buffer), MSG_PEEK, reinterpret_cast<sockaddr*>(&from),
                         &from_len) == 5);
    CHECK(VNet::recvfrom(u, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from),
                         &from_len) == 5);
    CHECK(std::memcmp(buffer, "hello", 5) == 0 && ntohs(from.sin_port) == echo.udp_port);
    CHECK(VNet::recvfrom(u, buffer, sizeof(buffer), MSG_DONTWAIT, nullptr, nullptr) == -1 &&
          VNet::LastError() == EWOULDBLOCK);
    // A private address: the Plaza drops it, the game sees a datagram sent into the void.
    auto lan = Loopback(9);
    lan.sin_addr.s_addr = htonl(0x0A000005);
    CHECK(VNet::sendto(u, "x", 1, 0, reinterpret_cast<sockaddr*>(&lan), sizeof(lan)) == 1);
    CHECK(VNet::close(u) == 0 && VNet::close(u) == -1);

    // TCP: a blocking connect, then a non-blocking one; both echo.
    const int t = VNet::socket(AF_INET, SOCK_STREAM, 0);
    auto server = Loopback(echo.tcp_port);
    CHECK(VNet::connect(t, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == 0);
    std::string big(100000, 'x');
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>('a' + i % 26);
    }
    CHECK(VNet::sendto(t, big.data(), big.size(), 0, nullptr, 0) == static_cast<int>(big.size()));
    std::string back;
    while (back.size() < big.size()) {
        char chunk[8192];
        const int n = VNet::recvfrom(t, chunk, sizeof(chunk), 0, nullptr, nullptr);
        if (n <= 0) {
            break;
        }
        back.append(chunk, n);
    }
    CHECK(back == big);
    CHECK(VNet::close(t) == 0);
    const int nb = VNet::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(VNet::set_blocking(nb, false) == 0);
    CHECK(VNet::connect(nb, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1 &&
          VNet::LastError() == EINPROGRESS);
    pollfd w{nb, POLLOUT, 0};
    CHECK(VNet::poll(&w, 1, 2000) == 1 && (w.revents & POLLOUT));
    int error = -1;
    socklen_t error_len = sizeof(error);
    CHECK(VNet::getsockopt(nb, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_len) ==
              0 &&
          error == 0);
    CHECK(VNet::sendto(nb, "ping", 4, 0, nullptr, 0) == 4);
    pollfd r{nb, POLLIN, 0};
    CHECK(VNet::poll(&r, 1, 2000) == 1);
    CHECK(VNet::recvfrom(nb, buffer, sizeof(buffer), 0, nullptr, nullptr) == 4);
    // A refused connection.
    const int refused = VNet::socket(AF_INET, SOCK_STREAM, 0);
    auto nobody = Loopback(1);
    CHECK(VNet::connect(refused, reinterpret_cast<sockaddr*>(&nobody), sizeof(nobody)) == -1 &&
          VNet::LastError() == ECONNREFUSED);
    VNet::close(refused);
    CHECK(VNet::listen(nb, 1) == -1);

    // The interface goes away: the open socket is dead, no new ones.
    Tamariba::Inet::SetInterface(nullptr);
    CHECK(VNet::recvfrom(nb, buffer, sizeof(buffer), 0, nullptr, nullptr) <= 0);
    CHECK(VNet::sendto(nb, "x", 1, 0, nullptr, 0) == -1);
    CHECK(VNet::socket(AF_INET, SOCK_STREAM, 0) == -1);
    VNet::close(nb);
}

void TestHttp(FakeExit& exit, Echo& echo) {
    Tamariba::Inet::SetInterface(&exit.iface);
    httplib::Server server;
    server.Get("/hello", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content("hello from " + req.get_header_value("Host"), "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread serving([&] { server.listen_after_bind(); });
    const u16 proxy = Tamariba::Inet::HttpProxyPort();
    CHECK(proxy != 0);
    const int connects = exit.tcp_connects;
    // What http:C does: the console's URL, through the proxy.
    httplib::Client client("pretendo.test", port);
    client.set_proxy("127.0.0.1", proxy);
    const auto result = client.Get("/hello");
    if (!result || result->status != 200) {
        std::fprintf(stderr, "HTTP through the proxy: %s, status %d\n",
                     httplib::to_string(result.error()).c_str(), result ? result->status : 0);
    }
    CHECK(result && result->status == 200);
    CHECK(result && result->body == "hello from pretendo.test:" + std::to_string(port));
    CHECK(exit.tcp_connects == connects + 1); // it went through the exit
    // A name the exit does not know: 502 from the proxy, never a direct connection.
    httplib::Client unknown("nintendo.test", port);
    unknown.set_proxy("127.0.0.1", proxy);
    const auto refused = unknown.Get("/hello");
    CHECK(!refused || refused->status == 502);
    // HTTPS goes as CONNECT: the bytes pass through untouched (here: to the echo server).
    const int raw = ::socket(AF_INET, SOCK_STREAM, 0);
    auto at = Loopback(proxy);
    CHECK(::connect(raw, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
    const std::string connect =
        "CONNECT pretendo.test:" + std::to_string(echo.tcp_port) + " HTTP/1.1\r\nHost: x\r\n\r\n";
    ::send(raw, connect.data(), connect.size(), 0);
    char answer[256] = {};
    timeval tv{5, 0};
    setsockopt(raw, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const auto n = ::recv(raw, answer, sizeof(answer) - 1, 0);
    CHECK(n > 0 && std::string(answer).rfind("HTTP/1.1 200", 0) == 0);
    ::send(raw, "tls?", 4, 0);
    char echoed[8] = {};
    CHECK(::recv(raw, echoed, sizeof(echoed), 0) == 4 && std::memcmp(echoed, "tls?", 4) == 0);
    ::close(raw);
    server.stop();
    serving.join();
    Tamariba::Inet::Shutdown();
    CHECK(Tamariba::Inet::HttpProxyPort() == 0);
}

} // namespace

int main() {
    FakeExit exit;
    Echo echo;
    TestNoNetwork();
    TestSockets(exit, echo);
    TestHttp(exit, echo);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("tamariba_inet_test: all passed (%d TCP connections, %d datagrams, %d lookups "
                "through the exit)\n",
                exit.tcp_connects.load(), exit.udp_datagrams.load(), exit.lookups_made.load());
    return 0;
}
