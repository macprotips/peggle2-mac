// Xbox networking backed by host UDP sockets that are bound to the loopback address only.
// Titles see a console on a LAN without Xbox Live; nothing ever leaves the machine.
#include "../thread.h"
#include "objects.h"
#include "xbox_types.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace rt
{
    enum : uint32_t
    {
        WSAEINTR = 10004,
        WSAEBADF = 10009,
        WSAEINVAL = 10022,
        WSAEWOULDBLOCK = 10035,
        WSAENOTSOCK = 10038,
        WSAEAFNOSUPPORT = 10047,
        WSAEADDRINUSE = 10048,
        WSAENETUNREACH = 10051,
        WSAHOST_NOT_FOUND = 11001,
        SOCKET_ERROR = 0xFFFFFFFF,
    };

    // XSOCKADDR_IN: family is big-endian, port and address are in network order (as on the wire).
    struct GuestSockaddrIn
    {
        be<uint16_t> family;
        uint8_t port[2];
        uint8_t address[4];
        uint8_t zero[8];
    };

    struct Socket
    {
        int fd = -1;
        bool nonBlocking = false;
        std::atomic<bool> closed{ false };
    };

    static std::mutex g_socketsMutex;
    static std::map<uint32_t, std::shared_ptr<Socket>> g_sockets;
    static uint32_t g_nextSocket = 0x5000;

    static std::shared_ptr<Socket> FindSocket(uint32_t handle)
    {
        std::lock_guard lock(g_socketsMutex);
        auto it = g_sockets.find(handle);
        return it == g_sockets.end() ? nullptr : it->second;
    }

    static uint32_t Fail(uint32_t wsaError)
    {
        SetGuestLastError(wsaError);
        return SOCKET_ERROR;
    }

    static uint32_t ErrnoToWsa(int error)
    {
        switch (error)
        {
        case EWOULDBLOCK: return WSAEWOULDBLOCK;
        case EADDRINUSE: return WSAEADDRINUSE;
        case EINVAL: return WSAEINVAL;
        case EBADF: return WSAEBADF;
        case ENETUNREACH: return WSAENETUNREACH;
        default: return WSAEINVAL;
        }
    }

    // Titles bind fixed ports below 1024 (eg. 1000), which macOS reserves for root. Traffic never
    // leaves loopback, so those ports are shifted into a private range on the host and shifted back
    // whenever an address is reported to the title.
    constexpr uint16_t PrivilegedPortShift = 40000;

    static uint16_t HostPort(uint16_t guestPort)
    {
        return guestPort != 0 && guestPort < 1024 ? uint16_t(guestPort + PrivilegedPortShift) : guestPort;
    }

    static uint16_t GuestPort(uint16_t hostPort)
    {
        return hostPort >= PrivilegedPortShift && hostPort < PrivilegedPortShift + 1024 ? uint16_t(hostPort - PrivilegedPortShift) : hostPort;
    }

    static sockaddr_in ToHost(const GuestSockaddrIn* guest)
    {
        sockaddr_in host{};
        host.sin_len = sizeof(host);
        host.sin_family = AF_INET;
        host.sin_port = htons(HostPort(uint16_t(guest->port[0] << 8 | guest->port[1])));
        memcpy(&host.sin_addr, guest->address, 4);
        return host;
    }

    static void ToGuest(const sockaddr_in& host, GuestSockaddrIn* guest)
    {
        memset(guest, 0, sizeof(*guest));
        guest->family = 2;
        uint16_t port = GuestPort(ntohs(host.sin_port));
        guest->port[0] = uint8_t(port >> 8);
        guest->port[1] = uint8_t(port);
        memcpy(guest->address, &host.sin_addr, 4);
    }

    static bool IsLoopback(const sockaddr_in& a)
    {
        return (ntohl(a.sin_addr.s_addr) >> 24) == 127;
    }

    static uint32_t NetDll_XNetStartup(uint32_t, uint32_t) { return 0; }
    static uint32_t NetDll_XNetCleanup(uint32_t, uint32_t) { return 0; }

    static uint32_t NetDll_WSAStartup(uint32_t, uint16_t, uint8_t* data)
    {
        if (data)
        {
            memset(data, 0, 0x190);
            *reinterpret_cast<be<uint16_t>*>(data) = 0x0202;     // wVersion
            *reinterpret_cast<be<uint16_t>*>(data + 2) = 0x0202; // wHighVersion
        }
        return 0;
    }

    static uint32_t NetDll_WSACleanup(uint32_t) { return 0; }

    static uint32_t NetDll_XNetGetEthernetLinkStatus(uint32_t)
    {
        return 0x01 | 0x02; // XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS
    }

    // XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
    static uint32_t NetDll_XNetGetTitleXnAddr(uint32_t, uint8_t* address)
    {
        memset(address, 0, 0x24);
        address[0] = 127;
        address[3] = 1;
        address[0x0A] = 0x00; // MAC 00:22:48:00:00:01, locally unique and stable
        address[0x0B] = 0x22;
        address[0x0C] = 0x48;
        address[0x0F] = 0x01;
        return 0x02 | 0x04; // XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_STATIC
    }

    static uint32_t NetDll_XNetXnAddrToInAddr(uint32_t, const uint8_t* xnaddr, uint32_t, uint8_t* inAddr)
    {
        memcpy(inAddr, xnaddr, 4);
        return 0;
    }

    static void NetDll_XNetInAddrToString(uint32_t, uint32_t inAddr, char* out, uint32_t size)
    {
        in_addr a;
        a.s_addr = htonl(inAddr);
        inet_ntop(AF_INET, &a, out, size);
    }

    static uint32_t NetDll_inet_addr(const char* text)
    {
        in_addr a;
        return inet_pton(AF_INET, text, &a) == 1 ? ntohl(a.s_addr) : 0xFFFFFFFF;
    }

    // XNDNS / XNQOS: allocated and completed immediately with no results.
    static uint32_t NetDll_XNetDnsLookup(uint32_t, const char* host, uint32_t event, be<uint32_t>* result)
    {
        uint32_t dns = g_systemHeap.Alloc(0x28);
        *Translate<be<uint32_t>>(dns) = WSAHOST_NOT_FOUND;
        *result = dns;
        if (event)
            SetEvent(HandleToObject(event));
        Log("XNetDnsLookup({}): offline", host ? host : "");
        return 0;
    }

    static uint32_t NetDll_XNetDnsRelease(uint32_t, uint32_t dns)
    {
        g_systemHeap.Free(dns);
        return 0;
    }

    static uint32_t NetDll_XNetQosServiceLookup(uint32_t, uint32_t, uint32_t event, be<uint32_t>* result)
    {
        uint32_t qos = g_systemHeap.Alloc(0x40);
        *result = qos;
        if (event)
            SetEvent(HandleToObject(event));
        return 0;
    }

    static uint32_t NetDll_XNetQosRelease(uint32_t, uint32_t qos)
    {
        g_systemHeap.Free(qos);
        return 0;
    }

    static uint32_t NetDll_socket(uint32_t, uint32_t af, uint32_t type, uint32_t protocol)
    {
        constexpr uint32_t SOCK_DGRAM_GUEST = 2;
        if (af != 2 || type != SOCK_DGRAM_GUEST)
        {
            Log("socket(af {}, type {}, protocol {}): only UDP/IPv4 is supported", af, type, protocol);
            return Fail(WSAEAFNOSUPPORT);
        }
        int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0)
            return Fail(ErrnoToWsa(errno));
        auto s = std::make_shared<Socket>();
        s->fd = fd;
        std::lock_guard lock(g_socketsMutex);
        uint32_t handle = g_nextSocket++;
        g_sockets[handle] = s;
        return handle;
    }

    static uint32_t NetDll_closesocket(uint32_t, uint32_t handle)
    {
        std::shared_ptr<Socket> s;
        {
            std::lock_guard lock(g_socketsMutex);
            auto it = g_sockets.find(handle);
            if (it == g_sockets.end())
                return Fail(WSAENOTSOCK);
            s = it->second;
            g_sockets.erase(it);
        }
        s->closed = true;
        close(s->fd);
        return 0;
    }

    static uint32_t NetDll_bind(uint32_t, uint32_t handle, const GuestSockaddrIn* name, uint32_t)
    {
        auto s = FindSocket(handle);
        if (!s)
            return Fail(WSAENOTSOCK);
        sockaddr_in a = ToHost(name);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // never listen beyond this machine
        if (::bind(s->fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
            return 0;
        // The port may be taken (a second copy of the game, or another app). Online play is off, and the
        // title's failure path is fragile (a failed bind once freed a live object), so take any free port.
        const int error = errno;
        a.sin_port = 0;
        if (error == EADDRINUSE && ::bind(s->fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
        {
            Log("bind: port in use, using a free port instead");
            return 0;
        }
        Log("bind to 127.0.0.1 failed: {}", strerror(error));
        return Fail(ErrnoToWsa(error));
    }

    static uint32_t NetDll_getsockname(uint32_t, uint32_t handle, GuestSockaddrIn* name, be<uint32_t>* length)
    {
        auto s = FindSocket(handle);
        if (!s)
            return Fail(WSAENOTSOCK);
        sockaddr_in a{};
        socklen_t n = sizeof(a);
        if (getsockname(s->fd, reinterpret_cast<sockaddr*>(&a), &n) != 0)
            return Fail(ErrnoToWsa(errno));
        ToGuest(a, name);
        *length = sizeof(GuestSockaddrIn);
        return 0;
    }

    static uint32_t NetDll_setsockopt(uint32_t, uint32_t handle, uint32_t level, uint32_t option, const be<uint32_t>* value, uint32_t)
    {
        auto s = FindSocket(handle);
        if (!s)
            return Fail(WSAENOTSOCK);
        constexpr uint32_t SOL_SOCKET_GUEST = 0xFFFF;
        int v = value ? int(uint32_t(*value)) : 0;
        if (level == SOL_SOCKET_GUEST)
        {
            switch (option)
            {
            case 0x0004: setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)); break;
            case 0x0020: setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &v, sizeof(v)); break;
            case 0x1001: setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)); break;
            case 0x1002: setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)); break;
            default: break; // Xbox-specific options (eg. insecure sockets) have no host meaning
            }
        }
        return 0;
    }

    static uint32_t NetDll_ioctlsocket(uint32_t, uint32_t handle, uint32_t command, be<uint32_t>* argument)
    {
        auto s = FindSocket(handle);
        if (!s)
            return Fail(WSAENOTSOCK);
        constexpr uint32_t GuestFIONBIO = 0x8004667E;
        constexpr uint32_t GuestFIONREAD = 0x4004667F;
        if (command == GuestFIONBIO)
        {
            s->nonBlocking = *argument != 0;
            return 0;
        }
        if (command == GuestFIONREAD)
        {
            int available = 0;
            ioctl(s->fd, FIONREAD, &available);
            *argument = uint32_t(available);
            return 0;
        }
        return Fail(WSAEINVAL);
    }

    static uint32_t NetDll_recvfrom(uint32_t, uint32_t handle, uint8_t* buffer, uint32_t length, uint32_t, GuestSockaddrIn* from, be<uint32_t>* fromLength)
    {
        auto s = FindSocket(handle);
        if (!s)
            return Fail(WSAENOTSOCK);
        // Blocking receives poll in short slices so closesocket can end them.
        while (!s->nonBlocking)
        {
            pollfd p{ s->fd, POLLIN, 0 };
            int ready = poll(&p, 1, 100);
            if (s->closed)
                return Fail(WSAEINTR);
            if (ready > 0)
                break;
        }
        sockaddr_in a{};
        socklen_t n = sizeof(a);
        // A syscall cannot take a write-watch fault: receive into the physical windows (where watches live)
        // through a host buffer.
        const bool physical = IsPhysicalWindow(ToGuest(buffer));
        thread_local std::vector<uint8_t> staging;
        if (physical)
            staging.resize(length);
        ssize_t got = ::recvfrom(s->fd, physical ? staging.data() : buffer, length, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&a), &n);
        if (got < 0)
            return Fail(ErrnoToWsa(errno));
        if (physical)
            memcpy(buffer, staging.data(), size_t(got));
        if (from)
            ToGuest(a, from);
        if (fromLength)
            *fromLength = sizeof(GuestSockaddrIn);
        return uint32_t(got);
    }

    static uint32_t SendTo(Socket& s, const uint8_t* data, uint32_t length, const GuestSockaddrIn* to)
    {
        sockaddr_in a = ToHost(to);
        if (!IsLoopback(a))
            return length; // offline: traffic for other machines is dropped, as on an unplugged LAN
        ssize_t sent = ::sendto(s.fd, data, length, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        return sent < 0 ? Fail(ErrnoToWsa(errno)) : uint32_t(sent);
    }

    static uint32_t NetDll_sendto(uint32_t, uint32_t handle, const uint8_t* buffer, uint32_t length, uint32_t, const GuestSockaddrIn* to, uint32_t)
    {
        auto s = FindSocket(handle);
        return s ? SendTo(*s, buffer, length, to) : Fail(WSAENOTSOCK);
    }

    struct WsaBuf
    {
        be<uint32_t> length;
        be<uint32_t> buffer;
    };

    PPC_FUNC(WSASendToImpl)
    {
        // (caller, socket, buffers, count, bytesSent*, flags, to, toLength, overlapped, completion)
        auto s = FindSocket(ctx.r4.u32);
        if (!s)
        {
            ctx.r3.u64 = Fail(WSAENOTSOCK);
            return;
        }
        auto* buffers = Translate<WsaBuf>(ctx.r5.u32);
        std::vector<uint8_t> data;
        for (uint32_t i = 0; i < ctx.r6.u32; i++)
        {
            const uint8_t* p = Translate<uint8_t>(buffers[i].buffer);
            data.insert(data.end(), p, p + uint32_t(buffers[i].length));
        }
        uint32_t sent = SendTo(*s, data.data(), uint32_t(data.size()), Translate<GuestSockaddrIn>(ctx.r9.u32));
        if (sent == SOCKET_ERROR)
        {
            ctx.r3.u64 = SOCKET_ERROR;
            return;
        }
        if (ctx.r7.u32)
            *Translate<be<uint32_t>>(ctx.r7.u32) = sent;
        ctx.r3.u64 = 0;
    }

    static uint32_t NetDll_WSACreateEvent()
    {
        return CreateHandle(CreateDispatcherObject(TypeNotificationEvent, 0x10, 0));
    }

    static uint32_t NetDll_WSACloseEvent(uint32_t handle)
    {
        return CloseHandle(handle) ? 1 : 0;
    }
}

using namespace rt;
GUEST_IMPORT(NetDll_XNetStartup, NetDll_XNetStartup)
GUEST_IMPORT(NetDll_WSAStartup, NetDll_WSAStartup)
GUEST_IMPORT(NetDll_WSACleanup, NetDll_WSACleanup)
GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus, NetDll_XNetGetEthernetLinkStatus)
GUEST_IMPORT(NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr)
GUEST_IMPORT(NetDll_XNetXnAddrToInAddr, NetDll_XNetXnAddrToInAddr)
GUEST_IMPORT(NetDll_XNetInAddrToString, NetDll_XNetInAddrToString)
GUEST_IMPORT(NetDll_inet_addr, NetDll_inet_addr)
GUEST_IMPORT(NetDll_XNetDnsLookup, NetDll_XNetDnsLookup)
GUEST_IMPORT(NetDll_XNetDnsRelease, NetDll_XNetDnsRelease)
GUEST_IMPORT(NetDll_XNetQosServiceLookup, NetDll_XNetQosServiceLookup)
GUEST_IMPORT(NetDll_XNetQosRelease, NetDll_XNetQosRelease)
GUEST_IMPORT(NetDll_socket, NetDll_socket)
GUEST_IMPORT(NetDll_closesocket, NetDll_closesocket)
GUEST_IMPORT(NetDll_bind, NetDll_bind)
GUEST_IMPORT(NetDll_getsockname, NetDll_getsockname)
GUEST_IMPORT(NetDll_setsockopt, NetDll_setsockopt)
GUEST_IMPORT(NetDll_ioctlsocket, NetDll_ioctlsocket)
GUEST_IMPORT(NetDll_recvfrom, NetDll_recvfrom)
GUEST_IMPORT(NetDll_sendto, NetDll_sendto)
GUEST_IMPORT(NetDll_WSACreateEvent, NetDll_WSACreateEvent)
GUEST_IMPORT(NetDll_WSACloseEvent, NetDll_WSACloseEvent)
PPC_FUNC(__imp__NetDll_WSASendTo) { rt::WSASendToImpl(ctx, base); }
