#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>
#include <chrono>
#include <algorithm>
#include <stdexcept>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using net_socket_t = SOCKET;
  static constexpr net_socket_t net_invalid_socket = INVALID_SOCKET;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
  using net_socket_t = int;
  static constexpr net_socket_t net_invalid_socket = -1;
#endif

namespace net {

struct Peer {
    std::uint32_t id = 0;
    std::string address;
    std::uint32_t rtt_ms = 0;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual void host(std::uint16_t port, int max_clients) = 0;
    virtual void connect(const std::string& addr, std::uint16_t port) = 0;
    virtual void send(std::uint32_t peer_id, const std::uint8_t* data, std::size_t len, bool reliable) = 0;
    virtual void poll(float timeout_ms,
        std::function<void(std::uint32_t peer_id, const std::uint8_t* data, std::size_t len)> on_recv,
        std::function<void(Peer)> on_connect,
        std::function<void(Peer)> on_disconnect) = 0;
    virtual void disconnect(std::uint32_t peer_id) = 0;
    virtual std::uint32_t get_rtt_ms(std::uint32_t peer_id) const = 0;
};

namespace detail {

inline std::string address_to_string(const sockaddr_storage& ss) {
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&ss), sizeof(ss),
                    host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        return std::string(host) + ":" + service;
    }
    return "unknown";
}

inline sockaddr_storage make_addr(const std::string& addr, std::uint16_t port) {
    sockaddr_storage out{};
    sockaddr_in* v4 = reinterpret_cast<sockaddr_in*>(&out);
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    if (inet_pton(AF_INET, addr.c_str(), &v4->sin_addr) == 1) return out;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(addr.c_str(), port_str.c_str(), &hints, &res) == 0 && res) {
        std::memcpy(&out, res->ai_addr, std::min<std::size_t>(sizeof(out), res->ai_addrlen));
        freeaddrinfo(res);
        return out;
    }
    return out;
}

inline std::uint64_t now_ms() {
    using namespace std::chrono;
    return (std::uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

template <class T>
inline void append_u32(std::vector<std::uint8_t>& out, T v) {
    std::uint32_t x = (std::uint32_t)v;
    for (int i = 0; i < 4; ++i) out.push_back((std::uint8_t)((x >> (i * 8)) & 0xFF));
}
inline void append_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back((std::uint8_t)(v & 0xFF));
    out.push_back((std::uint8_t)((v >> 8) & 0xFF));
}
inline void append_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}
inline std::uint32_t read_u32(const std::uint8_t* p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
inline std::uint16_t read_u16(const std::uint8_t* p) {
    return (std::uint16_t)p[0] | (std::uint16_t)(p[1] << 8);
}

} // namespace detail

class UdpTransport final : public Transport {
public:
    UdpTransport() = default;
    ~UdpTransport() override { close_socket(); }

    void host(std::uint16_t port, int max_clients) override {
        close_socket();
        open_socket();
        _host_mode = true;
        _client_mode = false;
        _max_clients = std::max(1, max_clients);
        _server_addr = {};
        _server_addr_len = 0;
        _peer_addrs.clear();
        _peer_states.clear();
        _addr_to_peer.clear();
        _peer_addr_strings.clear();
        _server_id = 0;
        _next_peer_id = 1;
        _peer_id = 0;
        _last_connect_send_ms = 0;
        _connected = true;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (::bind(_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("UdpTransport::host bind failed");
        }
    }

    void connect(const std::string& addr, std::uint16_t port) override {
        close_socket();
        open_socket();
        _host_mode = false;
        _client_mode = true;
        _connected = false;
        _peer_id = 0;
        _server_id = 0;
        _server_addr = detail::make_addr(addr, port);
        _server_addr_len = sizeof(sockaddr_in);
        _peer_addrs.clear();
        _peer_states.clear();

        // Client always talks to host via logical peer 0.
        _peer_addrs[0] = _server_addr;
        _peer_states[0] = PeerState{Peer{0, addr + ":" + std::to_string(port)}};

        send_connect();
        _last_connect_send_ms = detail::now_ms();
        _connect_started_ms = _last_connect_send_ms;
        _connect_failed = false;
    }

    void send(std::uint32_t peer_id, const std::uint8_t* data, std::size_t len, bool reliable) override {
        if (!_sock || peer_id == UINT32_MAX || !data || !len) return;
        auto it = _peer_addrs.find(peer_id);
        if (it == _peer_addrs.end()) return;

        Packet packet;
        packet.type = PacketType::Data;
        packet.sender_id = _peer_id;
        packet.receiver_id = peer_id;
        packet.reliable = reliable;
        packet.seq = reliable ? ++_peer_states[peer_id].next_out_seq : 0;
        packet.ack = _peer_states[peer_id].recv_ack;
        packet.ack_bits = _peer_states[peer_id].recv_mask;
        packet.payload.assign(data, data + len);

        if (reliable) {
            Pending pending;
            pending.packet = packet;
            pending.last_sent_ms = 0;
            _peer_states[peer_id].pending[packet.seq] = pending;
        }
        send_packet(peer_id, packet);
    }

    void poll(float timeout_ms,
        std::function<void(std::uint32_t peer_id, const std::uint8_t* data, std::size_t len)> on_recv,
        std::function<void(Peer)> on_connect,
        std::function<void(Peer)> on_disconnect) override
    {
        if (!_sock) return;

        const std::uint64_t now = detail::now_ms();

        // Client still trying to reach a host that's never answered at all
        // (wrong address/port, host down, firewalled). Give up after
        // _connect_timeout_ms instead of retrying Connect forever with the
        // caller never finding out.
        if (_client_mode && !_connected && !_connect_failed) {
            if (now - _connect_started_ms >= _connect_timeout_ms) {
                _connect_failed = true;
                if (on_disconnect && _peer_states.count(0)) on_disconnect(_peer_states[0].peer);
            } else if (now - _last_connect_send_ms >= 250) {
                send_connect();
                _last_connect_send_ms = now;
            }
        }

        // Retransmit pending reliable packets.
        for (auto& [peer_id, state] : _peer_states) {
            for (auto& [seq, pending] : state.pending) {
                if (pending.acked) continue;
                if (pending.last_sent_ms == 0 || now - pending.last_sent_ms >= _resend_interval_ms) {
                    send_packet(peer_id, pending.packet);
                    pending.last_sent_ms = now;
                }
            }
        }

        // Heartbeat: ping every known peer periodically, and drop any peer
        // we haven't heard a single packet from (ping, data, ack, anything)
        // in _peer_timeout_ms. This is what actually detects a host/client
        // that vanished uncleanly (crash, force-quit, cable pulled) instead
        // of leaving a dead connection looking alive forever.
        std::vector<std::uint32_t> timed_out;
        for (auto& [peer_id, state] : _peer_states) {
            if (_client_mode && !_connected && peer_id == 0) continue; // handled by the connect-timeout block above
            if (state.last_recv_ms == 0) state.last_recv_ms = now; // just connected; don't instantly time out
            if (now - state.last_recv_ms >= _peer_timeout_ms) {
                timed_out.push_back(peer_id);
                continue;
            }
            if (now - state.last_ping_sent_ms >= _ping_interval_ms) {
                send_ping(peer_id);
            }
        }
        for (std::uint32_t peer_id : timed_out) {
            Peer p = _peer_states[peer_id].peer;
            p.rtt_ms = _peer_states[peer_id].rtt_ms;
            remove_peer(peer_id);
            if (_client_mode && peer_id == 0) _connected = false;
            if (on_disconnect) on_disconnect(p);
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(_sock, &readfds);
        timeval tv{};
        timeval* ptv = nullptr;
        if (timeout_ms >= 0.f) {
            tv.tv_sec = (long)(timeout_ms / 1000.f);
            tv.tv_usec = (long)((timeout_ms - (float)tv.tv_sec * 1000.f) * 1000.f);
            ptv = &tv;
        }

        int ready = select((int)(_sock + 1), &readfds, nullptr, nullptr, ptv);
        if (ready <= 0) return;

        while (true) {
            std::array<std::uint8_t, 1400> buffer{};
            sockaddr_storage from{};
            socklen_t from_len = sizeof(from);
            int received = (int)recvfrom(_sock, reinterpret_cast<char*>(buffer.data()), (int)buffer.size(), 0,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (received <= 0) break;

            Packet packet;
            if (!decode(buffer.data(), (std::size_t)received, packet)) continue;

            handle_ack(packet.sender_id, packet.ack, packet.ack_bits);

            if (packet.type == PacketType::Connect) {
                if (_host_mode) {
                    std::string addr = detail::address_to_string(from);
                    bool was_new = _addr_to_peer.find(addr) == _addr_to_peer.end();
                    std::uint32_t id = assign_peer(from);
                    if (id == 0) continue;
                    _peer_states[id].last_recv_ms = now;
                    send_welcome(id);
                    if (was_new && on_connect) on_connect(_peer_states[id].peer);
                }
                continue;
            }

            auto peer_id = packet.sender_id;
            if (packet.type == PacketType::Welcome) {
                if (_client_mode) {
                    std::uint32_t assigned = packet.receiver_id ? packet.receiver_id : detail::read_u32(packet.payload.data());
                    if (assigned == 0) assigned = 1;
                    _peer_id = assigned;
                    _connected = true;
                    _peer_states[0].peer.id = assigned;
                    _peer_states[0].last_recv_ms = now;
                    if (on_connect) on_connect(_peer_states[0].peer);
                }
                continue;
            }

            if (packet.type == PacketType::Disconnect) {
                if (on_disconnect && _peer_states.count(peer_id)) on_disconnect(_peer_states[peer_id].peer);
                remove_peer(peer_id);
                continue;
            }

            if (!valid_peer(peer_id)) continue;
            auto& state = _peer_states[peer_id];
            state.last_recv_ms = now;

            if (packet.type == PacketType::Ping) {
                if (packet.payload.size() >= 4) {
                    std::uint32_t token = detail::read_u32(packet.payload.data());
                    send_pong(peer_id, token);
                }
                continue;
            }

            if (packet.type == PacketType::Pong) {
                if (packet.payload.size() >= 4 && state.last_ping_sent_ms != 0) {
                    std::uint32_t token = detail::read_u32(packet.payload.data());
                    if (token == state.last_ping_token) {
                        std::uint64_t rtt = now - state.last_ping_sent_ms;
                        state.rtt_ms = (std::uint32_t)std::min<std::uint64_t>(rtt, 0xFFFFFFFFull);
                        state.peer.rtt_ms = state.rtt_ms;
                    }
                }
                continue;
            }

            if (packet.type == PacketType::Data) {
                if (is_duplicate(state, packet.seq)) {
                    send_ack(peer_id);
                    continue;
                }
                mark_received(state, packet.seq);
                if (on_recv && !packet.payload.empty()) {
                    on_recv(peer_id, packet.payload.data(), packet.payload.size());
                }
                send_ack(peer_id);
                continue;
            }
        }
    }

    void disconnect(std::uint32_t peer_id) override {
        if (!_sock) return;
        auto it = _peer_addrs.find(peer_id);
        if (it == _peer_addrs.end()) return;
        Packet p;
        p.type = PacketType::Disconnect;
        p.sender_id = _peer_id;
        p.receiver_id = peer_id;
        p.reliable = false;
        send_packet(peer_id, p);
        remove_peer(peer_id);
    }

    std::uint32_t get_rtt_ms(std::uint32_t peer_id) const override {
        auto it = _peer_states.find(peer_id);
        return it != _peer_states.end() ? it->second.rtt_ms : 0;
    }

private:
    enum class PacketType : std::uint8_t { Connect = 1, Welcome = 2, Data = 3, Ack = 4, Disconnect = 5, Ping = 6, Pong = 7 };

    struct Packet {
        PacketType type = PacketType::Data;
        std::uint32_t sender_id = 0;
        std::uint32_t receiver_id = 0;
        std::uint32_t seq = 0;
        std::uint32_t ack = 0;
        std::uint32_t ack_bits = 0;
        bool reliable = false;
        std::vector<std::uint8_t> payload;
    };

    struct Pending {
        Packet packet;
        std::uint64_t last_sent_ms = 0;
        bool acked = false;
    };

    struct PeerState {
        Peer peer;
        std::uint32_t next_out_seq = 0;
        std::uint32_t recv_ack = 0;
        std::uint32_t recv_mask = 0;
        std::uint32_t last_recv_seq = 0;
        std::unordered_map<std::uint32_t, Pending> pending;
        std::uint64_t last_recv_ms = 0;      // any packet from this peer, for timeout detection
        std::uint64_t last_ping_sent_ms = 0;
        std::uint32_t last_ping_token = 0;
        std::uint32_t rtt_ms = 0;
    };

    net_socket_t _sock = net_invalid_socket;
    bool _host_mode = false;
    bool _client_mode = false;
    bool _connected = false;
    int _max_clients = 0;
    std::uint32_t _peer_id = 0;      // local assigned peer id (host=0, client gets assigned on welcome)
    std::uint32_t _server_id = 0;    // logical host on client side (always 0)
    std::uint32_t _next_peer_id = 1;  // host-assigned remote ids start at 1
    std::uint64_t _resend_interval_ms = 120;
    std::uint64_t _last_connect_send_ms = 0;
    std::uint64_t _connect_started_ms = 0;
    bool _connect_failed = false;
    std::uint64_t _ping_interval_ms = 1000;   // how often we ping each peer
    std::uint64_t _peer_timeout_ms = 8000;    // no packet at all from peer for this long => dead
    std::uint64_t _connect_timeout_ms = 8000; // client gives up retrying Connect after this long

    sockaddr_storage _server_addr{};
    socklen_t _server_addr_len = 0;

    std::unordered_map<std::uint32_t, sockaddr_storage> _peer_addrs;
    std::unordered_map<std::uint32_t, PeerState> _peer_states;
    std::unordered_map<std::string, std::uint32_t> _addr_to_peer;
    std::unordered_map<std::uint32_t, std::string> _peer_addr_strings;

    void open_socket() {
#if defined(_WIN32)
        static bool wsa_ok = false;
        if (!wsa_ok) {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
                throw std::runtime_error("UdpTransport: WSAStartup failed");
            }
            wsa_ok = true;
        }
#endif
        _sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_sock == net_invalid_socket) throw std::runtime_error("UdpTransport: socket() failed");
        set_nonblocking(true);

        // Without this, host() can fail with "bind failed" (EADDRINUSE) when
        // a previous session's socket on the same port hasn't been fully
        // released yet by the OS (e.g. after a quick restart, or a crash
        // that skipped Network::Shutdown()). This mirrors the same option
        // already used by DiscoverySocket elsewhere in matchmaking.hpp.
        int yes = 1;
#if defined(_WIN32)
        ::setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, (int)sizeof(yes));
#else
        ::setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&yes, (socklen_t)sizeof(yes));
#  if defined(SO_REUSEPORT)
        ::setsockopt(_sock, SOL_SOCKET, SO_REUSEPORT, (const void*)&yes, (socklen_t)sizeof(yes));
#  endif
#endif
    }

    void close_socket() {
        if (_sock == net_invalid_socket) return;
#if defined(_WIN32)
        closesocket(_sock);
#else
        close(_sock);
#endif
        _sock = net_invalid_socket;
    }

    void set_nonblocking(bool enabled) {
#if defined(_WIN32)
        u_long mode = enabled ? 1UL : 0UL;
        ioctlsocket(_sock, FIONBIO, &mode);
#else
        int flags = fcntl(_sock, F_GETFL, 0);
        if (flags >= 0) fcntl(_sock, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
    }

    static std::size_t header_size() { return 4 + 1 + 1 + 1 + 1 + 4 * 5 + 4; }

    static void encode(const Packet& p, std::vector<std::uint8_t>& out) {
        out.clear();
        out.reserve(header_size() + p.payload.size());
        out.push_back('G'); out.push_back('E'); out.push_back('2'); out.push_back('D');
        out.push_back(1); // version
        out.push_back((std::uint8_t)p.type);
        out.push_back(p.reliable ? 1 : 0);
        out.push_back(0);
        detail::append_u32(out, p.sender_id);
        detail::append_u32(out, p.receiver_id);
        detail::append_u32(out, p.seq);
        detail::append_u32(out, p.ack);
        detail::append_u32(out, p.ack_bits);
        detail::append_u32(out, (std::uint32_t)p.payload.size());
        out.insert(out.end(), p.payload.begin(), p.payload.end());
    }

    static bool decode(const std::uint8_t* data, std::size_t len, Packet& out) {
        if (!data || len < 4 + 1 + 1 + 1 + 1 + 4 * 5 + 4) return false;
        if (!(data[0]=='G' && data[1]=='E' && data[2]=='2' && data[3]=='D')) return false;
        std::size_t off = 4;
        std::uint8_t version = data[off++]; (void)version;
        out.type = (PacketType)data[off++];
        out.reliable = data[off++] != 0;
        ++off; // reserved
        out.sender_id   = detail::read_u32(data + off); off += 4;
        out.receiver_id = detail::read_u32(data + off); off += 4;
        out.seq         = detail::read_u32(data + off); off += 4;
        out.ack         = detail::read_u32(data + off); off += 4;
        out.ack_bits    = detail::read_u32(data + off); off += 4;
        std::uint32_t payload_len = detail::read_u32(data + off); off += 4;
        if (off + payload_len > len) return false;
        out.payload.assign(data + off, data + off + payload_len);
        return true;
    }

    static bool same_addr(const sockaddr_storage& a, const sockaddr_storage& b) {
        if (a.ss_family != b.ss_family) return false;
        if (a.ss_family == AF_INET) {
            auto* av4 = reinterpret_cast<const sockaddr_in*>(&a);
            auto* bv4 = reinterpret_cast<const sockaddr_in*>(&b);
            return av4->sin_port == bv4->sin_port && av4->sin_addr.s_addr == bv4->sin_addr.s_addr;
        }
#if defined(AF_INET6)
        if (a.ss_family == AF_INET6) {
            auto* av6 = reinterpret_cast<const sockaddr_in6*>(&a);
            auto* bv6 = reinterpret_cast<const sockaddr_in6*>(&b);
            return av6->sin6_port == bv6->sin6_port &&
                   std::memcmp(&av6->sin6_addr, &bv6->sin6_addr, sizeof(in6_addr)) == 0;
        }
#endif
        return false;
    }

    bool valid_peer(std::uint32_t id) const {
        return _peer_states.find(id) != _peer_states.end();
    }

    std::uint32_t assign_peer(const sockaddr_storage& from) {
        std::string addr = detail::address_to_string(from);
        auto it = _addr_to_peer.find(addr);
        if (it != _addr_to_peer.end()) return it->second;
        if ((int)_peer_states.size() >= _max_clients) return 0;
        std::uint32_t id = _next_peer_id++;
        _addr_to_peer[addr] = id;
        _peer_addrs[id] = from;
        _peer_addr_strings[id] = addr;
        _peer_states[id] = PeerState{Peer{id, addr}};
        return id;
    }

    void remove_peer(std::uint32_t id) {
        auto addr_it = _peer_addr_strings.find(id);
        if (addr_it != _peer_addr_strings.end()) {
            _addr_to_peer.erase(addr_it->second);
            _peer_addr_strings.erase(addr_it);
        }
        auto it = _peer_states.find(id);
        if (it != _peer_states.end()) _peer_states.erase(it);
        _peer_addrs.erase(id);
    }

    void send_packet(std::uint32_t peer_id, const Packet& p) {
        auto it = _peer_addrs.find(peer_id);
        if (it == _peer_addrs.end()) return;
        std::vector<std::uint8_t> buffer;
        encode(p, buffer);
        sockaddr_storage addr = it->second;
#if defined(AF_INET6)
        int addr_len = (addr.ss_family == AF_INET6) ? (int)sizeof(sockaddr_in6) : (int)sizeof(sockaddr_in);
#else
        int addr_len = (int)sizeof(sockaddr_in);
#endif
        ::sendto(_sock, reinterpret_cast<const char*>(buffer.data()), (int)buffer.size(), 0,
                 reinterpret_cast<sockaddr*>(&addr), addr_len);
    }

    void send_connect() {
        Packet p;
        p.type = PacketType::Connect;
        p.sender_id = 0;
        p.receiver_id = 0;
        send_packet(0, p);
    }

    void send_welcome(std::uint32_t peer_id) {
        Packet p;
        p.type = PacketType::Welcome;
        p.sender_id = 0;
        p.receiver_id = peer_id;
        p.payload.resize(4);
        std::uint32_t v = peer_id;
        p.payload[0] = (std::uint8_t)(v & 0xFF);
        p.payload[1] = (std::uint8_t)((v >> 8) & 0xFF);
        p.payload[2] = (std::uint8_t)((v >> 16) & 0xFF);
        p.payload[3] = (std::uint8_t)((v >> 24) & 0xFF);
        send_packet(peer_id, p);
    }

    void send_ack(std::uint32_t peer_id) {
        if (!_peer_states.count(peer_id)) return;
        auto& s = _peer_states[peer_id];
        Packet p;
        p.type = PacketType::Ack;
        p.sender_id = _peer_id;
        p.receiver_id = peer_id;
        p.ack = s.recv_ack;
        p.ack_bits = s.recv_mask;
        send_packet(peer_id, p);
    }

    // Heartbeat: payload carries a 4-byte token (the sender's local send
    // time, low 32 bits of ms) that the receiver echoes back unchanged in
    // the matching Pong. RTT is measured purely from that round trip, so it
    // doesn't depend on the reliable ack/resend machinery at all — it works
    // even if the link is otherwise idle.
    void send_ping(std::uint32_t peer_id) {
        auto it = _peer_states.find(peer_id);
        if (it == _peer_states.end()) return;
        std::uint64_t now = detail::now_ms();
        std::uint32_t token = (std::uint32_t)(now & 0xFFFFFFFFu);
        it->second.last_ping_sent_ms = now;
        it->second.last_ping_token = token;
        Packet p;
        p.type = PacketType::Ping;
        p.sender_id = _peer_id;
        p.receiver_id = peer_id;
        p.payload.resize(4);
        p.payload[0] = (std::uint8_t)(token & 0xFF);
        p.payload[1] = (std::uint8_t)((token >> 8) & 0xFF);
        p.payload[2] = (std::uint8_t)((token >> 16) & 0xFF);
        p.payload[3] = (std::uint8_t)((token >> 24) & 0xFF);
        send_packet(peer_id, p);
    }

    void send_pong(std::uint32_t peer_id, std::uint32_t token) {
        Packet p;
        p.type = PacketType::Pong;
        p.sender_id = _peer_id;
        p.receiver_id = peer_id;
        p.payload.resize(4);
        p.payload[0] = (std::uint8_t)(token & 0xFF);
        p.payload[1] = (std::uint8_t)((token >> 8) & 0xFF);
        p.payload[2] = (std::uint8_t)((token >> 16) & 0xFF);
        p.payload[3] = (std::uint8_t)((token >> 24) & 0xFF);
        send_packet(peer_id, p);
    }

    void handle_ack(std::uint32_t peer_id, std::uint32_t ack, std::uint32_t ack_bits) {
        auto it = _peer_states.find(peer_id);
        if (it == _peer_states.end()) return;
        auto& st = it->second;
        auto acknowledge = [&](std::uint32_t seq) {
            auto pit = st.pending.find(seq);
            if (pit != st.pending.end()) {
                pit->second.acked = true;
                st.pending.erase(pit);
            }
        };
        if (ack) acknowledge(ack);
        for (int i = 0; i < 32; ++i) {
            if (ack_bits & (1u << i)) {
                std::uint32_t seq = ack - 1u - (std::uint32_t)i;
                if (seq > 0) acknowledge(seq);
            }
        }
    }

    static bool is_duplicate(PeerState& state, std::uint32_t seq) {
        if (seq == 0) return false;
        if (seq > state.last_recv_seq) return false;
        std::uint32_t diff = state.last_recv_seq - seq;
        if (diff >= 32u) return true;
        return (state.recv_mask & (1u << diff)) != 0;
    }

    static void mark_received(PeerState& state, std::uint32_t seq) {
        if (seq == 0) return;
        if (seq > state.last_recv_seq) {
            std::uint32_t diff = seq - state.last_recv_seq;
            if (diff >= 32u) state.recv_mask = 0;
            else state.recv_mask = (state.recv_mask << diff) | 1u;
            state.last_recv_seq = seq;
            state.recv_ack = seq;
        } else {
            std::uint32_t diff = state.last_recv_seq - seq;
            if (diff < 32u) state.recv_mask |= (1u << diff);
        }
    }
};

inline std::unique_ptr<Transport> make_udp_transport() {
    return std::make_unique<UdpTransport>();
}

} // namespace net