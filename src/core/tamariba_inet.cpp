// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>
#include "common/logging/log.h"
#include "core/tamariba_inet.h"

#ifdef _WIN32
#define VE(x) WSA##x
#define VE_PIPE WSAESHUTDOWN // Winsock has no EPIPE
using NativeSocket = SOCKET;
constexpr NativeSocket NoSocket = INVALID_SOCKET;
#define native_close closesocket
#define native_poll WSAPoll
#else
#include <cerrno>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#define VE(x) x
#define VE_PIPE EPIPE
using NativeSocket = int;
constexpr NativeSocket NoSocket = -1;
#define native_close ::close
#define native_poll ::poll
#endif

namespace Tamariba::Inet {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::shared_mutex g_lock; ///< exclusive: SetInterface; shared: every call into the interface
const tamariba_inet_interface* g_inet = nullptr;
std::atomic<u64> g_epoch{1}; ///< grows with every SetInterface: sockets of an older one are dead

} // namespace

/// Runs f(interface) under the shared lock; `none` when there is no interface.
template <typename F, typename R>
R With(F&& f, R none) {
    std::shared_lock lock(g_lock);
    if (!g_inet) {
        return none;
    }
    return f(*g_inet);
}

u64 Epoch() {
    return g_epoch.load();
}

void SetInterface(const tamariba_inet_interface* inet) {
    std::unique_lock lock(g_lock);
    if (inet && inet->version < TAMARIBA_NET_VERSION) {
        LOG_ERROR(Network, "Tamariba: an internet interface of version {}: not used",
                  inet->version);
        inet = nullptr;
    }
    g_inet = inet;
    ++g_epoch;
    LOG_INFO(Network, "Tamariba: the console's internet {}",
             inet ? "goes through the Tamariba Plaza" : "is off");
}

bool Available() {
    std::shared_lock lock(g_lock);
    return g_inet != nullptr;
}

std::optional<InterfaceInfo> GetInterfaceInfo() {
    if (!Available()) {
        return std::nullopt;
    }
    // 10.0.0.2/24: private, so a game that tells others its local address tells them nothing.
    return InterfaceInfo{htonl(0x0A000002), htonl(0xFFFFFF00), htonl(0x0A0000FF)};
}

/// A name looked up through the interface (or a dotted address); network byte order.
std::optional<u32> Resolve(const std::string& host, std::chrono::milliseconds limit = 20s) {
    in_addr numeric{};
    if (inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
        return numeric.s_addr;
    }
    if (host.empty() || host.size() > 253) {
        return std::nullopt;
    }
    const int handle =
        With([&](const tamariba_inet_interface& i) { return i.resolve(i.ctx, host.c_str()); }, 0);
    if (handle <= 0) {
        return std::nullopt;
    }
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        u32 ip = 0;
        const int r = With(
            [&](const tamariba_inet_interface& i) { return i.resolve_result(i.ctx, handle, &ip); },
            static_cast<int>(TAMARIBA_INET_CLOSED));
        if (r == TAMARIBA_INET_OK) {
            return ip;
        }
        if (r != TAMARIBA_INET_AGAIN) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(2ms);
    }
    return std::nullopt;
}

// --- The HTTP proxy for http:C -----------------------------------------------------------------
//
// httplib (http_c.cpp) talks to it as to any HTTP proxy: "CONNECT host:443" for HTTPS (then
// TLS straight through), or "GET http://host/path" for plain HTTP. Each connection is carried
// by one TCP connection of the interface.

namespace {

class HttpProxy {
public:
    ~HttpProxy() {
        Stop();
    }
    u16 Port() {
        std::lock_guard lock(mutex);
        if (listener == NoSocket && !Start()) {
            return 0;
        }
        return port;
    }
    void Stop() {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex);
            stop = true;
            threads.swap(workers);
            if (accept_thread.joinable()) {
                threads.push_back(std::move(accept_thread));
            }
        }
        for (auto& t : threads) {
            t.join();
        }
        std::lock_guard lock(mutex);
        if (listener != NoSocket) {
            native_close(listener);
            listener = NoSocket;
        }
        port = 0;
        stop = false;
    }

private:
    bool Start() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == NoSocket) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener, 16) != 0 ||
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            native_close(listener);
            listener = NoSocket;
            return false;
        }
        port = ntohs(address.sin_port);
        accept_thread = std::thread([this] { AcceptLoop(); });
        LOG_INFO(Network, "Tamariba: HTTP of the console through 127.0.0.1:{}", port);
        return true;
    }

    void AcceptLoop() {
        while (!stop) {
            pollfd p{};
            p.fd = listener;
            p.events = POLLIN;
            if (native_poll(&p, 1, 100) <= 0) {
                continue;
            }
            const NativeSocket client = ::accept(listener, nullptr, nullptr);
            if (client == NoSocket) {
                continue;
            }
            std::lock_guard lock(mutex);
            workers.emplace_back([this, client] {
                Serve(client);
                native_close(client);
            });
        }
    }

    static bool SendAll(NativeSocket s, const char* data, std::size_t size) {
        while (size > 0) {
            const auto n = ::send(s, data, static_cast<int>(std::min<std::size_t>(size, 65536)), 0);
            if (n <= 0) {
                return false;
            }
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    void Serve(NativeSocket client) {
        // The request's head: up to the blank line.
        std::string head;
        char buffer[16384];
        const auto deadline = Clock::now() + 15s;
        while (head.find("\r\n\r\n") == std::string::npos) {
            if (stop || Clock::now() > deadline || head.size() > 32768) {
                return;
            }
            pollfd p{};
            p.fd = client;
            p.events = POLLIN;
            if (native_poll(&p, 1, 50) <= 0) {
                continue;
            }
            const auto n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return;
            }
            head.append(buffer, static_cast<std::size_t>(n));
        }
        const std::size_t line_end = head.find("\r\n");
        const std::string line = head.substr(0, line_end);
        const std::size_t a = line.find(' '), b = line.rfind(' ');
        if (a == std::string::npos || b == a) {
            return;
        }
        const std::string method = line.substr(0, a), target = line.substr(a + 1, b - a - 1);
        const bool tunnel = method == "CONNECT";
        std::string authority;
        u16 port = tunnel ? 443 : 80;
        if (tunnel) {
            authority = target;
        } else if (target.rfind("http://", 0) == 0) {
            authority = target.substr(7, target.find('/', 7) - 7);
        } else {
            return;
        }
        if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
            port = static_cast<u16>(std::strtoul(authority.c_str() + colon + 1, nullptr, 10));
            authority.resize(colon);
        }
        const auto fail = [&] {
            static const char bad[] =
                "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            SendAll(client, bad, sizeof(bad) - 1);
        };
        const auto ip = Resolve(authority);
        if (!ip || port == 0) {
            LOG_WARNING(Network, "Tamariba: the console's HTTP: no address for {}", authority);
            return fail();
        }
        const int handle =
            With([&](const tamariba_inet_interface& i) { return i.tcp_connect(i.ctx, *ip, port); },
                 static_cast<int>(TAMARIBA_INET_CLOSED));
        if (handle <= 0) {
            return fail();
        }
        const auto close_remote = [&] {
            With(
                [&](const tamariba_inet_interface& i) {
                    i.tcp_close(i.ctx, handle);
                    return 0;
                },
                0);
        };
        int state = TAMARIBA_INET_AGAIN;
        const u64 epoch = Epoch();
        for (const auto end = Clock::now() + 30s; state == TAMARIBA_INET_AGAIN && !stop;) {
            state =
                With([&](const tamariba_inet_interface& i) { return i.tcp_state(i.ctx, handle); },
                     static_cast<int>(TAMARIBA_INET_CLOSED));
            if (Clock::now() > end) {
                state = TAMARIBA_INET_ERROR;
            }
            std::this_thread::sleep_for(1ms);
        }
        if (state != TAMARIBA_INET_OK) {
            LOG_WARNING(Network, "Tamariba: the console's HTTP: {}:{} did not answer ({})",
                        authority, port, state);
            close_remote();
            return fail();
        }
        // Sends to the server through the interface, all of it (it may take less at a time).
        const auto to_remote = [&](const char* data, std::size_t size) {
            while (size > 0 && !stop && Epoch() == epoch) {
                const int n = With(
                    [&](const tamariba_inet_interface& i) {
                        return i.tcp_send(i.ctx, handle, data, size);
                    },
                    static_cast<int>(TAMARIBA_INET_CLOSED));
                if (n > 0) {
                    data += n;
                    size -= static_cast<std::size_t>(n);
                } else if (n == TAMARIBA_INET_AGAIN || n == 0) {
                    std::this_thread::sleep_for(1ms);
                } else {
                    return false;
                }
            }
            return size == 0;
        };
        if (tunnel) {
            static const char ok[] = "HTTP/1.1 200 Connection established\r\n\r\n";
            if (!SendAll(client, ok, sizeof(ok) - 1) ||
                !to_remote(head.data() + head.find("\r\n\r\n") + 4,
                           head.size() - head.find("\r\n\r\n") - 4)) {
                return close_remote();
            }
        } else {
            // "GET http://host/path" becomes "GET /path", as servers expect from a client
            // (http:C asks for one request per connection: the rest passes as it is).
            const std::size_t path_at = target.find('/', 7);
            const std::string origin =
                method + " " + (path_at == std::string::npos ? "/" : target.substr(path_at)) +
                line.substr(b);
            if (!to_remote(origin.data(), origin.size()) ||
                !to_remote(head.data() + line_end, head.size() - line_end)) {
                return close_remote();
            }
        }
        // Both ways until either side closes.
        while (!stop && Epoch() == epoch) {
            bool moved = false;
            pollfd p{};
            p.fd = client;
            p.events = POLLIN;
            if (native_poll(&p, 1, 0) > 0) {
                const auto n = ::recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0 || !to_remote(buffer, static_cast<std::size_t>(n))) {
                    break;
                }
                moved = true;
            }
            const int n = With(
                [&](const tamariba_inet_interface& i) {
                    return i.tcp_recv(i.ctx, handle, buffer, sizeof(buffer));
                },
                static_cast<int>(TAMARIBA_INET_CLOSED));
            if (n > 0) {
                if (!SendAll(client, buffer, static_cast<std::size_t>(n))) {
                    break;
                }
                moved = true;
            } else if (n != TAMARIBA_INET_AGAIN && n != 0) {
                break; // the server closed (or the exit went away)
            }
            if (!moved) {
                std::this_thread::sleep_for(1ms);
            }
        }
        close_remote();
    }

    std::mutex mutex;
    std::atomic<bool> stop{false};
    NativeSocket listener = NoSocket;
    u16 port = 0;
    std::thread accept_thread;
    std::vector<std::thread> workers;
};

HttpProxy g_proxy;

} // namespace

u16 HttpProxyPort() {
    return Available() ? g_proxy.Port() : 0;
}

} // namespace Tamariba::Inet

// --- The console's sockets -----------------------------------------------------------------------

namespace Tamariba::VNet {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Inet::Epoch;
using Inet::With;

thread_local int t_error = 0;

int Fail(int error) {
    t_error = error;
    return -1;
}

struct Datagram {
    u32 ip = 0;   ///< network byte order
    u16 port = 0; ///< host byte order
    std::vector<u8> data;
};

struct Socket {
    std::mutex mutex;
    bool tcp = false;
    bool blocking = true;
    u64 epoch = 0;
    int handle = 0;     ///< the interface's (0: none yet)
    u16 local_port = 0; ///< host byte order
    u32 peer_ip = 0;    ///< network byte order
    u16 peer_port = 0;  ///< host byte order
    enum class State { Idle, Connecting, Connected, Failed } state = State::Idle;
    int error = 0;                   ///< SO_ERROR
    std::vector<u8> in;              ///< TCP: received, not read yet
    bool eof = false;                ///< TCP: the other side closed
    std::optional<Datagram> pending; ///< UDP: arrived, not read yet
    bool shut_rd = false, shut_wr = false;
};

std::mutex g_sockets_mutex;
std::map<int, std::shared_ptr<Socket>> g_sockets;
int g_next_fd = 3;
u16 g_next_port = 49152;

std::shared_ptr<Socket> Find(int fd) {
    std::lock_guard lock(g_sockets_mutex);
    const auto it = g_sockets.find(fd);
    return it == g_sockets.end() ? nullptr : it->second;
}

u16 EphemeralPort() {
    std::lock_guard lock(g_sockets_mutex);
    const u16 port = g_next_port;
    g_next_port = g_next_port >= 65000 ? 49152 : g_next_port + 1;
    return port;
}

int MapStatus(int status) {
    switch (status) {
    case TAMARIBA_INET_REFUSED:
        return VE(ECONNREFUSED);
    case TAMARIBA_INET_CLOSED:
        return VE(ECONNRESET);
    default:
        return VE(ENETUNREACH);
    }
}

/// Moves what the interface has for `s` into it (s.mutex held).
void Pump(Socket& s) {
    if (s.epoch != Epoch()) {
        // The interface went away (or changed): this socket's handle means nothing any more.
        if (s.handle || s.state == Socket::State::Connecting) {
            s.handle = 0;
            s.eof = true;
            if (s.tcp && s.state != Socket::State::Idle) {
                s.state = Socket::State::Failed;
                s.error = VE(ENETDOWN);
            }
        }
        return;
    }
    if (!s.handle) {
        return;
    }
    if (s.tcp) {
        if (s.state == Socket::State::Connecting) {
            const int st =
                With([&](const tamariba_inet_interface& i) { return i.tcp_state(i.ctx, s.handle); },
                     static_cast<int>(TAMARIBA_INET_CLOSED));
            if (st == TAMARIBA_INET_OK) {
                s.state = Socket::State::Connected;
            } else if (st != TAMARIBA_INET_AGAIN) {
                s.state = Socket::State::Failed;
                s.error = MapStatus(st);
            }
        }
        if (s.state == Socket::State::Connected && !s.eof) {
            u8 buffer[16384];
            while (s.in.size() < 256 * 1024) {
                const int n = With(
                    [&](const tamariba_inet_interface& i) {
                        return i.tcp_recv(i.ctx, s.handle, buffer, sizeof(buffer));
                    },
                    static_cast<int>(TAMARIBA_INET_CLOSED));
                if (n > 0) {
                    s.in.insert(s.in.end(), buffer, buffer + n);
                    continue;
                }
                if (n != TAMARIBA_INET_AGAIN && n != 0) {
                    s.eof = true;
                    if (n != TAMARIBA_INET_CLOSED) {
                        s.error = VE(ECONNRESET);
                    }
                }
                break;
            }
        }
    } else if (!s.pending) {
        Datagram d;
        d.data.resize(2048);
        const int n = With(
            [&](const tamariba_inet_interface& i) {
                return i.udp_recvfrom(i.ctx, s.handle, &d.ip, &d.port, d.data.data(),
                                      d.data.size());
            },
            static_cast<int>(TAMARIBA_INET_CLOSED));
        if (n >= 0) {
            d.data.resize(static_cast<std::size_t>(n));
            s.pending = std::move(d);
        }
    }
}

bool Readable(const Socket& s) {
    return s.tcp ? (!s.in.empty() || s.eof || s.state == Socket::State::Failed)
                 : s.pending.has_value();
}

bool Writable(const Socket& s) {
    return s.tcp ? (s.state == Socket::State::Connected || s.state == Socket::State::Failed) : true;
}

/// UDP: the interface's handle, opened on first use (s.mutex held).
bool OpenUdp(Socket& s) {
    if (s.handle && s.epoch == Epoch()) {
        return true;
    }
    if (!s.local_port) {
        s.local_port = EphemeralPort();
    }
    s.epoch = Epoch();
    const int h =
        With([&](const tamariba_inet_interface& i) { return i.udp_open(i.ctx, s.local_port); },
             static_cast<int>(TAMARIBA_INET_CLOSED));
    if (h <= 0) {
        t_error = h == TAMARIBA_INET_CLOSED ? VE(ENETDOWN) : VE(EADDRINUSE);
        return false;
    }
    s.handle = h;
    return true;
}

bool ToInet(const sockaddr* addr, socklen_t len, u32& ip, u16& port) {
    if (!addr || len < static_cast<socklen_t>(sizeof(sockaddr_in)) || addr->sa_family != AF_INET) {
        return false;
    }
    const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
    ip = in->sin_addr.s_addr;
    port = ntohs(in->sin_port);
    return true;
}

void FromInet(u32 ip, u16 port, sockaddr* addr, socklen_t* len) {
    if (!addr || !len) {
        return;
    }
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = ip;
    in.sin_port = htons(port);
    std::memcpy(addr, &in, std::min<std::size_t>(sizeof(in), static_cast<std::size_t>(*len)));
    *len = sizeof(in);
}

/// Waits (s.mutex held, released while waiting) until `ready` holds or `limit` passes.
template <typename Ready>
bool WaitFor(Socket& s, std::unique_lock<std::mutex>& lock, Ready ready,
             std::chrono::milliseconds limit) {
    const auto end = Clock::now() + limit;
    while (true) {
        Pump(s);
        if (ready()) {
            return true;
        }
        if (Clock::now() >= end) {
            return false;
        }
        lock.unlock();
        std::this_thread::sleep_for(1ms);
        lock.lock();
    }
}

bool DontWait(int flags) {
#ifdef MSG_DONTWAIT
    return (flags & MSG_DONTWAIT) != 0;
#else
    (void)flags;
    return false;
#endif
}

} // namespace

int LastError() {
    return t_error;
}

int socket(int domain, int type, int protocol) {
    if (!Inet::Available()) {
        return Fail(VE(ENETDOWN));
    }
    if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM)) {
        return Fail(VE(EAFNOSUPPORT));
    }
    (void)protocol;
    auto s = std::make_shared<Socket>();
    s->tcp = type == SOCK_STREAM;
    s->epoch = Epoch();
    std::lock_guard lock(g_sockets_mutex);
    if (g_sockets.size() >= 64) {
        return Fail(VE(EMFILE));
    }
    while (g_sockets.count(g_next_fd)) {
        g_next_fd = g_next_fd >= 0x7FFF ? 3 : g_next_fd + 1;
    }
    const int fd = g_next_fd++;
    g_sockets[fd] = std::move(s);
    return fd;
}

int bind(int fd, const sockaddr* addr, socklen_t len) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    u32 ip = 0;
    u16 port = 0;
    if (!ToInet(addr, len, ip, port)) {
        return Fail(VE(EAFNOSUPPORT));
    }
    std::lock_guard lock(s->mutex);
    if (s->handle || s->local_port) {
        return Fail(VE(EINVAL));
    }
    s->local_port = port ? port : EphemeralPort();
    if (!s->tcp && !OpenUdp(*s)) {
        return -1;
    }
    return 0;
}

int listen(int, int) {
    // The exit carries connections the console makes, not ones made to it.
    return Fail(VE(EOPNOTSUPP));
}

int accept(int, sockaddr*, socklen_t*) {
    return Fail(VE(EOPNOTSUPP));
}

int connect(int fd, const sockaddr* addr, socklen_t len) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    u32 ip = 0;
    u16 port = 0;
    if (!ToInet(addr, len, ip, port)) {
        return Fail(VE(EAFNOSUPPORT));
    }
    std::unique_lock lock(s->mutex);
    if (!s->tcp) {
        if (!OpenUdp(*s)) {
            return -1;
        }
        s->peer_ip = ip;
        s->peer_port = port;
        return 0;
    }
    switch (s->state) {
    case Socket::State::Connected:
        return Fail(VE(EISCONN));
    case Socket::State::Connecting:
        if (!s->blocking) {
            Pump(*s);
            if (s->state == Socket::State::Connected) {
                return Fail(VE(EISCONN));
            }
            if (s->state == Socket::State::Connecting) {
                return Fail(VE(EALREADY));
            }
            const int error = s->error;
            s->error = 0;
            return Fail(error);
        }
        break;
    case Socket::State::Failed:
        return Fail(VE(ECONNREFUSED));
    case Socket::State::Idle: {
        s->epoch = Epoch();
        const int h =
            With([&](const tamariba_inet_interface& i) { return i.tcp_connect(i.ctx, ip, port); },
                 static_cast<int>(TAMARIBA_INET_CLOSED));
        if (h <= 0) {
            return Fail(h == TAMARIBA_INET_CLOSED ? VE(ENETDOWN) : MapStatus(h));
        }
        s->handle = h;
        s->peer_ip = ip;
        s->peer_port = port;
        if (!s->local_port) {
            s->local_port = EphemeralPort();
        }
        s->state = Socket::State::Connecting;
        if (!s->blocking) {
            return Fail(VE(EINPROGRESS));
        }
        break;
    }
    }
    if (!WaitFor(*s, lock, [&] { return s->state != Socket::State::Connecting; }, 30s)) {
        return Fail(VE(ETIMEDOUT));
    }
    if (s->state == Socket::State::Connected) {
        return 0;
    }
    const int error = s->error ? s->error : VE(ECONNREFUSED);
    s->error = 0;
    return Fail(error);
}

int sendto(int fd, const char* data, std::size_t len, int flags, const sockaddr* to,
           socklen_t to_len) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    std::unique_lock lock(s->mutex);
    const bool wait = s->blocking && !DontWait(flags);
    if (!s->tcp) {
        u32 ip = s->peer_ip;
        u16 port = s->peer_port;
        if (to && to_len > 0 && !ToInet(to, to_len, ip, port)) {
            return Fail(VE(EAFNOSUPPORT));
        }
        if (!port) {
            return Fail(VE(EDESTADDRREQ));
        }
        if (len > 1472) {
            return Fail(VE(EMSGSIZE));
        }
        if (!OpenUdp(*s)) {
            return -1;
        }
        const auto end = Clock::now() + (wait ? 1000ms : 0ms);
        while (true) {
            const int n = With(
                [&](const tamariba_inet_interface& i) {
                    return i.udp_sendto(i.ctx, s->handle, ip, port, data, len);
                },
                static_cast<int>(TAMARIBA_INET_CLOSED));
            if (n >= 0 || n == TAMARIBA_INET_REFUSED) {
                // Refused by the Plaza (a private or forbidden address): lost, as on a real
                // network.
                return static_cast<int>(len);
            }
            if (n != TAMARIBA_INET_AGAIN) {
                return Fail(n == TAMARIBA_INET_CLOSED ? VE(ENETDOWN) : VE(ENETUNREACH));
            }
            if (Clock::now() >= end) {
                return Fail(VE(EWOULDBLOCK));
            }
            lock.unlock();
            std::this_thread::sleep_for(1ms);
            lock.lock();
        }
    }
    Pump(*s);
    if (s->state != Socket::State::Connected || s->shut_wr) {
        return Fail(s->state == Socket::State::Failed || s->shut_wr ? VE_PIPE : VE(ENOTCONN));
    }
    std::size_t sent = 0;
    const auto end = Clock::now() + 30s;
    while (sent < len) {
        const int n = With(
            [&](const tamariba_inet_interface& i) {
                return i.tcp_send(i.ctx, s->handle, data + sent, len - sent);
            },
            static_cast<int>(TAMARIBA_INET_CLOSED));
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n != TAMARIBA_INET_AGAIN && n != 0) {
            if (sent > 0) {
                break;
            }
            return Fail(VE(ECONNRESET));
        }
        if (!wait || Clock::now() >= end || s->epoch != Epoch()) {
            if (sent > 0) {
                break;
            }
            return Fail(wait ? VE(ETIMEDOUT) : VE(EWOULDBLOCK));
        }
        lock.unlock();
        std::this_thread::sleep_for(1ms);
        lock.lock();
    }
    return static_cast<int>(sent);
}

int recvfrom(int fd, char* data, std::size_t len, int flags, sockaddr* from, socklen_t* from_len) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    std::unique_lock lock(s->mutex);
    const bool wait = s->blocking && !DontWait(flags);
    const bool peek = (flags & MSG_PEEK) != 0;
    if (s->tcp && s->state != Socket::State::Connected && s->state != Socket::State::Failed) {
        return Fail(VE(ENOTCONN));
    }
    if (!s->tcp && !OpenUdp(*s)) {
        return -1;
    }
    // A blocking read waits as long as it takes (a game ends it by closing or shutting down).
    const auto ready = [&] { return Readable(*s) || s->shut_rd || s->epoch != Epoch(); };
    if (!WaitFor(*s, lock, ready, wait ? std::chrono::hours(24) : 0ms)) {
        return Fail(VE(EWOULDBLOCK));
    }
    if (s->tcp) {
        if (s->in.empty()) {
            if (s->error) {
                const int error = s->error;
                s->error = 0;
                return Fail(error);
            }
            return 0; // closed
        }
        const std::size_t n = std::min(len, s->in.size());
        std::memcpy(data, s->in.data(), n);
        if (!peek) {
            s->in.erase(s->in.begin(), s->in.begin() + static_cast<std::ptrdiff_t>(n));
        }
        FromInet(s->peer_ip, s->peer_port, from, from_len);
        return static_cast<int>(n);
    }
    if (!s->pending) {
        return Fail(s->epoch != Epoch() ? VE(ENETDOWN) : VE(EWOULDBLOCK));
    }
    const Datagram& d = *s->pending;
    const std::size_t n = std::min(len, d.data.size());
    std::memcpy(data, d.data.data(), n);
    FromInet(d.ip, d.port, from, from_len);
    if (!peek) {
        s->pending.reset();
    }
    return static_cast<int>(n);
}

int poll(pollfd* fds, unsigned long nfds, int timeout_ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    while (true) {
        int count = 0;
        for (unsigned long i = 0; i < nfds; ++i) {
            fds[i].revents = 0;
            const auto s = Find(static_cast<int>(fds[i].fd));
            if (!s) {
                fds[i].revents = POLLNVAL;
                ++count;
                continue;
            }
            std::lock_guard lock(s->mutex);
            Pump(*s);
            short revents = 0;
            if ((fds[i].events & POLLIN) && (Readable(*s) || s->shut_rd)) {
                revents |= POLLIN;
            }
            if ((fds[i].events & POLLOUT) && Writable(*s)) {
                revents |= POLLOUT;
            }
            if (s->tcp && s->state == Socket::State::Failed) {
                revents |= POLLERR;
            }
            if (s->tcp && s->eof && s->in.empty()) {
                revents |= POLLHUP;
            }
            fds[i].revents = revents;
            count += revents != 0;
        }
        if (count > 0 || (timeout_ms >= 0 && Clock::now() >= end)) {
            return count;
        }
        std::this_thread::sleep_for(1ms);
    }
}

int getsockname(int fd, sockaddr* addr, socklen_t* len) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    std::lock_guard lock(s->mutex);
    const auto info = Inet::GetInterfaceInfo();
    FromInet(s->handle && info ? info->address : 0, s->local_port, addr, len);
    return 0;
}

int getpeername(int fd, sockaddr* addr, socklen_t* len) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    std::lock_guard lock(s->mutex);
    if (!s->peer_port || (s->tcp && s->state != Socket::State::Connected)) {
        return Fail(VE(ENOTCONN));
    }
    FromInet(s->peer_ip, s->peer_port, addr, len);
    return 0;
}

int getsockopt(int fd, int level, int name, char* value, socklen_t* len) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    if (!value || !len || *len < static_cast<socklen_t>(sizeof(int))) {
        return Fail(VE(EINVAL));
    }
    std::lock_guard lock(s->mutex);
    std::memset(value, 0, static_cast<std::size_t>(*len));
    int out = 0;
    if (level == SOL_SOCKET) {
        if (name == SO_ERROR) {
            Pump(*s);
            out = s->error;
            s->error = 0;
        } else if (name == SO_TYPE) {
            out = s->tcp ? SOCK_STREAM : SOCK_DGRAM;
        } else if (name == SO_RCVBUF || name == SO_SNDBUF) {
            out = 65536;
        } else if (name == SO_LINGER) {
            return 0; // off
        }
    }
    std::memcpy(value, &out, sizeof(out));
    *len = sizeof(out);
    return 0;
}

int setsockopt(int fd, int, int, const char*, socklen_t) {
    // Buffer sizes, TTL, no-delay and the like: the exit decides those.
    return Find(fd) ? 0 : Fail(VE(EBADF));
}

int shutdown(int fd, int how) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    std::lock_guard lock(s->mutex);
    if (s->tcp && s->state != Socket::State::Connected) {
        return Fail(VE(ENOTCONN));
    }
#ifdef _WIN32
    const bool rd = how == SD_RECEIVE || how == SD_BOTH, wr = how == SD_SEND || how == SD_BOTH;
#else
    const bool rd = how == SHUT_RD || how == SHUT_RDWR, wr = how == SHUT_WR || how == SHUT_RDWR;
#endif
    s->shut_rd = s->shut_rd || rd;
    s->shut_wr = s->shut_wr || wr;
    return 0;
}

int close(int fd) {
    std::shared_ptr<Socket> s;
    {
        std::lock_guard lock(g_sockets_mutex);
        const auto it = g_sockets.find(fd);
        if (it == g_sockets.end()) {
            return Fail(VE(EBADF));
        }
        s = it->second;
        g_sockets.erase(it);
    }
    std::lock_guard lock(s->mutex);
    if (s->handle && s->epoch == Epoch()) {
        With(
            [&](const tamariba_inet_interface& i) {
                s->tcp ? i.tcp_close(i.ctx, s->handle) : i.udp_close(i.ctx, s->handle);
                return 0;
            },
            0);
    }
    s->handle = 0;
    return 0;
}

int set_blocking(int fd, bool blocking) {
    const auto s = Find(fd);
    if (!s) {
        return Fail(VE(EBADF));
    }
    std::lock_guard lock(s->mutex);
    s->blocking = blocking;
    return 0;
}

int sockatmark(int fd) {
    return Find(fd) ? 0 : Fail(VE(EBADF));
}

hostent* gethostbyname(const char* name) {
    struct Entry {
        hostent h{};
        char name[256]{};
        in_addr address{};
        char* addresses[2]{};
        char* aliases[1]{};
    };
    thread_local Entry e;
    if (!name || !Inet::Available()) {
        t_error = VE(ENETDOWN);
        return nullptr;
    }
    const auto ip = Inet::Resolve(name);
    if (!ip) {
        t_error = VE(EHOSTUNREACH);
        return nullptr;
    }
    std::snprintf(e.name, sizeof(e.name), "%s", name);
    e.address.s_addr = *ip;
    e.addresses[0] = reinterpret_cast<char*>(&e.address);
    e.addresses[1] = nullptr;
    e.aliases[0] = nullptr;
    e.h.h_name = e.name;
    e.h.h_aliases = e.aliases;
    e.h.h_addrtype = AF_INET;
    e.h.h_length = sizeof(in_addr);
    e.h.h_addr_list = e.addresses;
    return &e.h;
}

hostent* gethostbyaddr(const char*, int, int) {
    // No reverse lookups through the exit.
    t_error = Inet::Available() ? VE(EHOSTUNREACH) : VE(ENETDOWN);
    return nullptr;
}

namespace {
struct AddrInfoBlock {
    addrinfo info;
    sockaddr_in address;
};
} // namespace

int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    if (!res) {
        return EAI_FAIL;
    }
    *res = nullptr;
    const int flags = hints ? hints->ai_flags : 0;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) {
        return EAI_FAMILY;
    }
    u16 port = 0;
    if (service && *service) {
        char* end = nullptr;
        const unsigned long p = std::strtoul(service, &end, 10);
        if (*end != '\0' || p > 65535) {
            return EAI_SERVICE;
        }
        port = static_cast<u16>(p);
    }
    u32 ip = 0;
    if (!node || !*node) {
        ip = (flags & AI_PASSIVE) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    } else {
        in_addr numeric{};
        if (inet_pton(AF_INET, node, &numeric) == 1) {
            ip = numeric.s_addr;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else if (!Inet::Available()) {
            return EAI_FAIL; // no network
        } else if (const auto found = Inet::Resolve(node)) {
            ip = *found;
        } else {
            return EAI_NONAME;
        }
    }
    const int types[2] = {SOCK_STREAM, SOCK_DGRAM};
    addrinfo** tail = res;
    for (const int type : types) {
        if (hints && hints->ai_socktype != 0 && hints->ai_socktype != type) {
            continue;
        }
        auto* block = new AddrInfoBlock{};
        block->address.sin_family = AF_INET;
        block->address.sin_addr.s_addr = ip;
        block->address.sin_port = htons(port);
        block->info.ai_family = AF_INET;
        block->info.ai_socktype = type;
        block->info.ai_protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
        block->info.ai_addrlen = sizeof(sockaddr_in);
        block->info.ai_addr = reinterpret_cast<sockaddr*>(&block->address);
        *tail = &block->info;
        tail = &block->info.ai_next;
    }
    return *res ? 0 : EAI_SOCKTYPE;
}

void freeaddrinfo(addrinfo* res) {
    while (res) {
        addrinfo* next = res->ai_next;
        delete reinterpret_cast<AddrInfoBlock*>(res); // the block begins with its addrinfo
        res = next;
    }
}

int getnameinfo(const sockaddr* addr, socklen_t len, char* host, std::size_t host_len, char* serv,
                std::size_t serv_len, int flags) {
    u32 ip = 0;
    u16 port = 0;
    if (!ToInet(addr, len, ip, port)) {
        return EAI_FAMILY;
    }
    if (flags & NI_NAMEREQD) {
        return EAI_NONAME; // only numbers: no reverse lookups through the exit
    }
    if (host && host_len > 0) {
        in_addr a{};
        a.s_addr = ip;
        if (!inet_ntop(AF_INET, &a, host, static_cast<socklen_t>(host_len))) {
            return EAI_FAIL;
        }
    }
    if (serv && serv_len > 0) {
        std::snprintf(serv, serv_len, "%u", port);
    }
    return 0;
}

} // namespace Tamariba::VNet

namespace Tamariba::Inet {

void Shutdown() {
    SetInterface(nullptr); // first: whatever still waits on the interface gives up at once
    g_proxy.Stop();
    std::lock_guard lock(VNet::g_sockets_mutex);
    VNet::g_sockets.clear();
}

} // namespace Tamariba::Inet
