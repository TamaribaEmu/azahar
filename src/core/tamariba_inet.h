// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Tamariba: the emulated console's own internet traffic (Pretendo Network) never touches the
// host's network. Tamariba hands the core an interface to the Tamariba Plaza's internet exit
// (common/tamariba_net.h, retro_tamariba_set_inet), and everything the console sends leaves
// through it, so nobody the game meets online learns the player's address:
//
//   SOC (soc:U, the console's sockets) calls the functions in Tamariba::VNet below instead of
//   the host's: they have the shape of the BSD socket calls soc_u.cpp makes, on sockets of
//   their own carried by the interface;
//   HTTP (http:C) sends every request through HttpProxyPort(), a proxy on 127.0.0.1 that
//   carries it through the interface (TLS stays end to end, between the console and the server).
//
// Without an interface (the default, and whenever online services are off) the console has
// no network at all: sockets cannot be made, names do not resolve, HTTP requests fail.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include "common/common_types.h"
#include "common/tamariba_net.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace Tamariba::Inet {

/// The frontend's interface (nullptr: none). Waits for calls in progress; afterwards nothing
/// touches the previous interface again, and every socket made through it is dead.
void SetInterface(const tamariba_inet_interface* inet);
bool Available();
/// Stops the HTTP proxy and drops every socket (retro_deinit).
void Shutdown();

/// 127.0.0.1:<port> of the HTTP proxy (started on first use); 0 without an interface.
u16 HttpProxyPort();

/// The console's network as games see it (soc:U GetHostId, NETOPT_IP_INFO): a private
/// address of its own (the exit's public address is not the console's); nothing when offline.
struct InterfaceInfo {
    u32 address;   ///< network byte order
    u32 netmask;   ///< network byte order
    u32 broadcast; ///< network byte order
};
std::optional<InterfaceInfo> GetInterfaceInfo();

} // namespace Tamariba::Inet

namespace Tamariba::VNet {

#ifdef _WIN32
using socklen_t = int;
#endif

/// The error of the last call that failed on this thread, as errno (WSAGetLastError on
/// Windows) would give it.
int LastError();

// The BSD socket calls soc_u.cpp makes (AF_INET only), on the console's own sockets.
// Descriptors are small numbers of their own, never host sockets.
int socket(int domain, int type, int protocol);
int bind(int fd, const sockaddr* addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, sockaddr* addr, socklen_t* len);
int connect(int fd, const sockaddr* addr, socklen_t len);
int sendto(int fd, const char* data, std::size_t len, int flags, const sockaddr* to,
           socklen_t to_len);
int recvfrom(int fd, char* data, std::size_t len, int flags, sockaddr* from, socklen_t* from_len);
int poll(pollfd* fds, unsigned long nfds, int timeout_ms);
int getsockname(int fd, sockaddr* addr, socklen_t* len);
int getpeername(int fd, sockaddr* addr, socklen_t* len);
int getsockopt(int fd, int level, int name, char* value, socklen_t* len);
int setsockopt(int fd, int level, int name, const char* value, socklen_t len);
int shutdown(int fd, int how);
int close(int fd);
int set_blocking(int fd, bool blocking);
int sockatmark(int fd);

hostent* gethostbyname(const char* name);
hostent* gethostbyaddr(const char* addr, int len, int type);
int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res);
void freeaddrinfo(addrinfo* res);
int getnameinfo(const sockaddr* addr, socklen_t len, char* host, std::size_t host_len, char* serv,
                std::size_t serv_len, int flags);

} // namespace Tamariba::VNet
