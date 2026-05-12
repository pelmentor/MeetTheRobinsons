// NetSession — Phase 1.4b UDP transport implementation.
//
// See mtr/coop/net/net_session.h for design rationale and threading
// contract. This file is the ONLY translation unit that pulls in
// <winsock2.h>; the header keeps it out via uintptr_t storage so any
// other TU including net_session.h doesn't risk a winsock2/windows.h
// include-order trap.

// winsock2.h MUST come before windows.h — and the project's CMake
// defines WIN32_LEAN_AND_MEAN so windows.h doesn't drag in winsock.h
// itself, but other transitive includes might. Explicit order here.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "mtr/coop/net/net_session.h"
#include "mtr/cmdline_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop::net {

namespace {

// Cap on cmdline value buffer for the -host PORT and -connect IP:PORT
// values. IPv4 dotted-quad + ':' + 5-digit port + NUL = 22 bytes max.
// 64 is comfortable headroom and matches the cmdline_utils contract.
constexpr std::size_t kValueBufSize = 64;

// Ethernet MTU. Our pose snapshot is 60 bytes today; sizing the recv
// buffer this large protects against future packet types (and against
// silent truncation if a malformed peer sends an oversize datagram).
constexpr std::size_t kRecvBufSize = 1500;

// Parse the value following `-mtrasi-coop-host` as a uint16_t port.
// Returns 0 on parse failure (which is also an invalid port — caller
// rejects). Leading/trailing whitespace already handled by
// cmdline_utils::get_flag_value; this just runs strtoul.
uint16_t parse_port(const char* s) {
    if (!s || !*s) return 0;
    char* endp = nullptr;
    const unsigned long v = std::strtoul(s, &endp, 10);
    if (!endp || *endp != '\0') return 0;  // trailing garbage
    if (v == 0 || v > 65535) return 0;
    return static_cast<uint16_t>(v);
}

// Parse `IP:PORT` form. On success, fills out_ip4 (network byte order)
// and out_port, returns true. On any malformation, returns false. Uses
// inet_pton (not inet_addr — inet_addr is ambiguous on 255.255.255.255).
bool parse_ip_port(const char* s, uint32_t& out_ip4, uint16_t& out_port) {
    if (!s || !*s) return false;

    const char* colon = std::strchr(s, ':');
    if (!colon || colon == s) return false;     // missing or empty IP

    char ip_buf[32];
    const std::size_t ip_len = static_cast<std::size_t>(colon - s);
    if (ip_len >= sizeof(ip_buf)) return false;
    std::memcpy(ip_buf, s, ip_len);
    ip_buf[ip_len] = '\0';

    in_addr a{};
    if (inet_pton(AF_INET, ip_buf, &a) != 1) return false;

    const uint16_t port = parse_port(colon + 1);
    if (port == 0) return false;

    out_ip4  = a.s_addr;
    out_port = port;
    return true;
}

// Read cmdline once, determine mode + host port (host) / peer addr
// (client). Magic-static cache. Disabled mode = neither flag present.
struct CmdlineConfig {
    SessionMode mode      = SessionMode::Disabled;
    uint16_t    host_port = 0;
    uint32_t    peer_ip4  = 0;
    uint16_t    peer_port = 0;
};

const CmdlineConfig& cmdline_config() {
    static const CmdlineConfig s_cfg = []{
        CmdlineConfig c{};
        LPSTR cl = GetCommandLineA();
        if (!cl) return c;

        char val[kValueBufSize];
        if (mtr::cmdline_utils::get_flag_value(cl, "-mtrasi-coop-host",
                                               val, sizeof(val))) {
            const uint16_t port = parse_port(val);
            if (port == 0) {
                mtr::log::info("[net_session] cmdline: -mtrasi-coop-host "
                               "value '%s' not a valid port (1..65535) "
                               "-- session DISABLED", val);
                return c;
            }
            c.mode      = SessionMode::Host;
            c.host_port = port;
            return c;
        }
        if (mtr::cmdline_utils::get_flag_value(cl, "-mtrasi-coop-connect",
                                               val, sizeof(val))) {
            uint32_t ip = 0;
            uint16_t port = 0;
            if (!parse_ip_port(val, ip, port)) {
                mtr::log::info("[net_session] cmdline: -mtrasi-coop-connect "
                               "value '%s' not a valid IP:PORT -- session "
                               "DISABLED", val);
                return c;
            }
            c.mode      = SessionMode::Client;
            c.peer_ip4  = ip;
            c.peer_port = port;
            return c;
        }
        return c;  // Disabled
    }();
    return s_cfg;
}

// Format a peer sockaddr_in for logs as "1.2.3.4:5678".
void format_addr(const sockaddr_in& sa, char* out, std::size_t cap) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    std::snprintf(out, cap, "%s:%u", ip, ntohs(sa.sin_port));
}

} // anon

NetSession& NetSession::instance() {
    static NetSession s;
    return s;
}

NetSession::~NetSession() {
    // Process-exit path: per dllmain convention the manager dtor is
    // called from static-destruction order, BEFORE the OS terminates
    // other threads. Stop the recv thread cleanly here so the post-this
    // MtrPlayerManager dtor (which acquires m_mu) can never deadlock
    // against a still-live recv thread sitting in on_remote_packet.
    // Reviewer P14 fix.
    shutdown();
}

void NetSession::set_recv_callback(RecvCallback cb) {
    // Read-by-recv-thread; intended one-time set at init. No mutex —
    // caller responsibility to set before install(). Documented in .h.
    m_recv_cb = std::move(cb);
}

bool NetSession::install() {
    const auto& cfg = cmdline_config();
    if (cfg.mode == SessionMode::Disabled) {
        // Not an error: user didn't opt into co-op.
        return true;
    }

    WSADATA wsa{};
    if (int e = WSAStartup(MAKEWORD(2, 2), &wsa); e != 0) {
        mtr::log::info("[net_session] WSAStartup failed (err=%d) -- session DISABLED", e);
        m_mode = SessionMode::Disabled;
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        const int err = WSAGetLastError();
        mtr::log::info("[net_session] socket() failed (WSAerr=%d) -- session DISABLED", err);
        WSACleanup();
        m_mode = SessionMode::Disabled;
        return false;
    }

    // Non-blocking send side: WSAEWOULDBLOCK on full send buffer is
    // preferable to blocking the sim thread. The recv loop uses a
    // blocking recvfrom on the SAME socket — non-blocking applies to
    // both directions for a UDP socket, so we use SO_RCVTIMEO instead
    // to keep recv blocking but the send-side WSAEWOULDBLOCK path
    // reachable. Actually simpler: set FIONBIO on the socket and the
    // recv loop polls with a short timeout via a select wrapper.
    //
    // Cleaner approach used here: keep the socket in default (blocking)
    // mode. Sendto on localhost UDP for 60-byte payloads is effectively
    // non-blocking in practice. The "theoretical blocking under buffer
    // pressure" hazard is addressed by the dedicated send-error counter
    // and rate-limited logging; if it ever fires in practice we move to
    // a non-blocking send-only socket pair. Per Principle 5: no
    // pre-emptive complexity.
    //
    // We DO set SO_RCVTIMEO short enough that shutdown can interrupt
    // the recv loop within a bounded time even when closesocket() from
    // another thread doesn't reliably wake recvfrom on Windows.

    constexpr DWORD kRecvTimeoutMs = 250;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&kRecvTimeoutMs),
                   sizeof(kRecvTimeoutMs)) == SOCKET_ERROR) {
        // Non-fatal — log + continue.
        mtr::log::info("[net_session] setsockopt(SO_RCVTIMEO) failed (WSAerr=%d) "
                       "-- continuing without recv timeout",
                       WSAGetLastError());
    }

    if (cfg.mode == SessionMode::Host) {
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(cfg.host_port);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            mtr::log::info("[net_session] bind(0.0.0.0:%u) failed (WSAerr=%d) "
                           "-- session DISABLED. Port likely in use.",
                           static_cast<unsigned>(cfg.host_port), err);
            closesocket(s);
            WSACleanup();
            m_mode = SessionMode::Disabled;
            return false;
        }
        mtr::log::info("[net_session] HOST: bound 0.0.0.0:%u, waiting for "
                       "peer's first datagram", static_cast<unsigned>(cfg.host_port));
    } else { // Client
        sockaddr_in peer{};
        peer.sin_family      = AF_INET;
        peer.sin_addr.s_addr = cfg.peer_ip4;          // already network byte order
        peer.sin_port        = htons(cfg.peer_port);
        // connect() on a UDP socket: sets default destination for send
        // and filters recv to that peer at the kernel level. This is
        // the right semantic for a single-peer 2-player session
        // (Principle 5). Side benefit: defends against rogue UDP on
        // the bound port.
        if (connect(s, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            mtr::log::info("[net_session] connect() to peer failed (WSAerr=%d) "
                           "-- session DISABLED", err);
            closesocket(s);
            WSACleanup();
            m_mode = SessionMode::Disabled;
            return false;
        }
        // Cache the peer addr so send() has a target without re-asking
        // the kernel. Same byte layout as host's m_peer_addr (which is
        // populated on first recv); both modes converge here.
        std::memcpy(m_peer_addr, &peer, sizeof(peer));
        m_peer_known.store(true, std::memory_order_release);
        char buf[32];
        format_addr(peer, buf, sizeof(buf));
        mtr::log::info("[net_session] CLIENT: connected to %s", buf);
    }

    m_sock = static_cast<uintptr_t>(s);
    m_mode = cfg.mode;

    m_recv_thread = CreateThread(nullptr, 0, &recv_thread_proc, this, 0, nullptr);
    if (!m_recv_thread) {
        mtr::log::info("[net_session] CreateThread for recv loop failed -- "
                       "session DISABLED");
        closesocket(s);
        WSACleanup();
        m_sock = static_cast<uintptr_t>(INVALID_SOCKET);
        m_mode = SessionMode::Disabled;
        return false;
    }

    return true;
}

void NetSession::shutdown() {
    if (m_mode == SessionMode::Disabled && m_recv_thread == nullptr) return;

    m_stopping.store(true, std::memory_order_release);

    const SOCKET s = static_cast<SOCKET>(m_sock);
    if (s != INVALID_SOCKET) {
        closesocket(s);  // recv thread will exit on next iteration / error
        m_sock = static_cast<uintptr_t>(INVALID_SOCKET);
    }

    if (m_recv_thread) {
        // Bounded wait: 2s is well past the 250ms SO_RCVTIMEO worst-case.
        WaitForSingleObject(m_recv_thread, 2000);
        CloseHandle(m_recv_thread);
        m_recv_thread = nullptr;
    }

    // WSACleanup balances WSAStartup. Process-exit path runs this from
    // the dtor; FreeLibrary path runs it from the explicit shutdown.
    // Only call if we actually had a session up.
    if (m_mode != SessionMode::Disabled) {
        WSACleanup();
    }
    m_mode = SessionMode::Disabled;
}

uint8_t NetSession::local_wire_id() const noexcept {
    switch (m_mode) {
        case SessionMode::Host:   return 0;
        case SessionMode::Client: return 1;
        default:                  return 0;
    }
}

uint8_t NetSession::remote_wire_id() const noexcept {
    switch (m_mode) {
        case SessionMode::Host:   return 1;
        case SessionMode::Client: return 0;
        default:                  return 0;
    }
}

void NetSession::send(const uint8_t* bytes, std::size_t len) {
    if (m_mode == SessionMode::Disabled) return;
    if (!bytes || len == 0)              return;
    const SOCKET s = static_cast<SOCKET>(m_sock);
    if (s == INVALID_SOCKET)             return;

    int sent = 0;
    if (m_mode == SessionMode::Client) {
        // connect() already set the default destination.
        sent = ::send(s, reinterpret_cast<const char*>(bytes),
                      static_cast<int>(len), 0);
    } else {
        // Host: only send once we've learnt the peer addr from a recv.
        if (!m_peer_known.load(std::memory_order_acquire)) return;
        sockaddr_in peer{};
        std::memcpy(&peer, m_peer_addr, sizeof(peer));
        sent = sendto(s, reinterpret_cast<const char*>(bytes),
                      static_cast<int>(len), 0,
                      reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    }
    if (sent == SOCKET_ERROR) {
        m_send_errors.fetch_add(1, std::memory_order_relaxed);
        // Rate-limited error log: roughly every 60 errors (= 1s at 60Hz
        // pose tick) so a disconnected peer doesn't flood the log. Read-
        // modify-write is single-threaded (sim thread only calls send())
        // so a plain check on m_send_errors after fetch_add is fine.
        const uint64_t n = m_send_errors.load(std::memory_order_relaxed);
        if (n == 1 || (n % 60) == 0) {
            mtr::log::info("[net_session] send error #%llu (WSAerr=%d)",
                           static_cast<unsigned long long>(n),
                           WSAGetLastError());
        }
    } else {
        m_packets_sent.fetch_add(1, std::memory_order_relaxed);
    }
}

DWORD WINAPI NetSession::recv_thread_proc(LPVOID param) {
    static_cast<NetSession*>(param)->recv_loop();
    return 0;
}

void NetSession::recv_loop() {
    uint8_t buf[kRecvBufSize];
    sockaddr_in from{};
    int from_len = sizeof(from);

    mtr::log::info("[net_session] recv thread started");

    while (!m_stopping.load(std::memory_order_acquire)) {
        const SOCKET s = static_cast<SOCKET>(m_sock);
        if (s == INVALID_SOCKET) break;

        from_len = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(buf),
                               static_cast<int>(kRecvBufSize), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                // Expected: SO_RCVTIMEO fired, poll the stop flag.
                continue;
            }
            // Per reviewer P2: Windows closesocket() doesn't reliably
            // deliver WSAEINTR; treat ANY non-timeout error as a stop
            // condition if shutdown is in progress, otherwise log+continue.
            if (m_stopping.load(std::memory_order_acquire)) break;
            mtr::log::info("[net_session] recvfrom error (WSAerr=%d)", err);
            // Avoid hot-spinning on a persistent error.
            Sleep(50);
            continue;
        }
        if (n == 0) continue;

        m_packets_recvd.fetch_add(1, std::memory_order_relaxed);

        // Host: learn peer addr from the first datagram. Once known,
        // ignore peer changes (a 2-player session has exactly one peer;
        // a different sender on the same port is suspicious and we
        // simply don't switch addr to them — they could still inject
        // pose packets but they can't redirect our send target).
        if (m_mode == SessionMode::Host &&
            !m_peer_known.load(std::memory_order_acquire)) {
            std::memcpy(m_peer_addr, &from, sizeof(from));
            m_peer_known.store(true, std::memory_order_release);
            char abuf[32];
            format_addr(from, abuf, sizeof(abuf));
            mtr::log::info("[net_session] HOST: learnt peer addr %s", abuf);
        }

        if (m_recv_cb) {
            const bool accepted = m_recv_cb(buf, static_cast<std::size_t>(n));
            if (!accepted) {
                m_bad_packets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // Intentionally silent on exit. The recv-thread teardown path on
    // FreeLibrary races against mtr::log::shutdown() in DllMain's
    // detach branch; logging here from the post-DllMain static-dtor
    // sweep is a use-after-shutdown on log. Audit fix 2026-05-12-c
    // (F1-correctness, 88% conf). The "started" log above is safe —
    // it fires during install() which always runs while log is up.
}

} // namespace mtr::coop::net
