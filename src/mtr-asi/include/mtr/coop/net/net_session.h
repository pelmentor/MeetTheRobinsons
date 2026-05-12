// NetSession — Phase 1.4b UDP transport for co-op.
//
// Owns one Winsock UDP socket per process and a dedicated recv thread.
// Knows nothing about packet types or game state: deals in byte buffers.
// Encode/decode happens in callers (MtrPlayerManager owns "what packets
// mean" — that's the Principle 7 boundary).
//
// MTA precedent for the pure-transport split:
//   reference/mtasa-blue/Client/sdk/net/CNet.h
//     -> SendPacket(ucPacketID, NetBitStream, ...)
//     -> RegisterPacketHandler(pfnPacketHandler)
//   CNet knows nothing about PACKET_ID_PLAYER_PURESYNC; the switch on
//   ucPacketID lives in CPacketHandler::ProcessPacket (deathmatch/logic),
//   not in CNet. NetSession follows the same shape.
//
// Mode selection — cmdline:
//   -mtrasi-coop-host PORT          => host mode (bind 0.0.0.0:PORT,
//                                      learn peer addr from first recv)
//   -mtrasi-coop-connect IP:PORT    => client mode (connect to peer, then
//                                      sendto/recvfrom filter to that peer)
//   neither                          => Disabled, install() is a no-op
//
// Threading:
//   - send() is called from the sim thread (MtrPlayerManager::do_pulse,
//     OUTSIDE manager m_mu). Non-blocking sendto; WSAEWOULDBLOCK counted
//     and dropped rather than blocking sim. This is the RULE-No-1 fix for
//     the otherwise-theoretical "sendto under buffer pressure could
//     freeze sim" hazard.
//   - recv runs on its own thread, blocking recvfrom loop. On every
//     received datagram the registered callback fires (still on the recv
//     thread). The callback parses the packet and dispatches into
//     MtrPlayerManager::on_remote_packet, which acquires m_mu internally.
//   - Shutdown sequence (only reached on explicit FreeLibrary; process
//     exit skips it per the dllmain convention): set m_stopping atomic,
//     closesocket, WaitForSingleObject(recv_thread, 2s), WSACleanup.
//     This pattern handles the Windows quirk that closesocket() doesn't
//     reliably wake a blocked recvfrom with WSAEINTR — we check m_stopping
//     after recvfrom returns *any* error.
//
// Architectural principles:
//   #5 minimum viable subset  — IPv4 only, one peer, no retry / reliability
//                                layer, magic-byte guard only (defense
//                                against unrelated UDP on the port).
//   #7 wrapper vs gameplay/net — this class is pure I/O, knows zero engine
//                                or gameplay state.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <windows.h>  // HANDLE typedef

namespace mtr::coop::net {

enum class SessionMode : uint8_t {
    Disabled = 0,   // no cmdline flag — install() is a no-op
    Host     = 1,   // bind a port, accept first peer that talks to us
    Client   = 2,   // connect() to the host's IP:PORT
};

// Callback type for received datagrams. Fires on the recv thread for
// EVERY received UDP datagram; the callback decides whether it's a
// valid mtr-asi packet (correct magic + version + a known packet
// type) and signals back via the return value: true = accepted,
// false = malformed/unknown (recv_loop bumps m_bad_packets). The
// pointer is into a recv-thread-local buffer — must not be retained
// past callback return.
//
// Returning bool keeps NetSession at the Principle-7 wrapper layer:
// the transport owns its diagnostic counters; the gameplay layer
// (callback in dllmain) only votes pass/fail per packet, never writes
// transport state directly. Audit fix 2026-05-12-c (F3-domain).
using RecvCallback = std::function<bool(const uint8_t* bytes, std::size_t len)>;

class NetSession {
public:
    static NetSession& instance();

    // Set the recv callback. MUST be called BEFORE install() — the recv
    // thread reads m_recv_cb once at startup; replacing it after the
    // thread is running is not thread-safe in this MVP (deliberate
    // Principle-5 omission; one site sets the callback at init time).
    void set_recv_callback(RecvCallback cb);

    // Read cmdline, open + bind/connect the socket, start the recv thread.
    // Returns true on success (including Disabled, which is success because
    // it means the user did not opt into co-op). Returns false on a hard
    // Winsock failure when the user *did* request co-op — caller may log
    // but the host process should not refuse to launch over this.
    // Idempotent.
    bool install();

    // Stops the recv thread and tears down the socket. Only relevant on
    // the FreeLibrary path (dllmain skips cleanup on process exit per
    // convention). Safe to call when install() was never called or was
    // a no-op.
    void shutdown();

    // Send raw bytes to the peer.
    //   - Host mode: no-op if peer addr not yet known (waiting for first
    //                client packet). Sendto with stored peer addr.
    //   - Client mode: send() on the connected socket.
    //   - Disabled / no socket: no-op.
    // Non-blocking under the hood; WSAEWOULDBLOCK is silently dropped
    // (counted in send_errors). The sim thread MUST NOT block here.
    void send(const uint8_t* bytes, std::size_t len);

    SessionMode mode()   const noexcept { return m_mode; }
    bool        active() const noexcept { return m_mode != SessionMode::Disabled; }

    // True once the recv thread has accepted at least one valid datagram
    // from the peer. Host mode: corresponds to "first packet learned us
    // the peer's addr". Client mode: corresponds to "host has started
    // replying to our packets" (since we sent first per the MVP). Used by
    // the coop-lan-soak test scenario to gate the soak phase on peer
    // presence before asserting fires_p2 == 0.
    bool peer_known() const noexcept { return m_peer_known.load(std::memory_order_acquire); }

    // Wire-format player id for the local player on this side of the
    // session. Host = 0, Client = 1, Disabled = 0 (unused). Mirrors the
    // 2-player MVP assumption: the wire id is determined by session role,
    // NOT by registration order, so both endpoints agree on which packet
    // belongs to whom regardless of which side registered its local
    // wilbur first.
    uint8_t local_wire_id()  const noexcept;
    uint8_t remote_wire_id() const noexcept;

    // Diagnostic counters. Lock-free atomics so callers (menu, log
    // heartbeat) can read at any time.
    uint64_t packets_sent()   const noexcept { return m_packets_sent.load(std::memory_order_relaxed); }
    uint64_t packets_recvd()  const noexcept { return m_packets_recvd.load(std::memory_order_relaxed); }
    uint64_t send_errors()    const noexcept { return m_send_errors.load(std::memory_order_relaxed); }
    uint64_t bad_packets()    const noexcept { return m_bad_packets.load(std::memory_order_relaxed); }

private:
    NetSession()  = default;
    ~NetSession();

    NetSession(const NetSession&)            = delete;
    NetSession& operator=(const NetSession&) = delete;

    static DWORD WINAPI recv_thread_proc(LPVOID param);
    void recv_loop();

    SessionMode             m_mode          = SessionMode::Disabled;

    // SOCKET is UINT_PTR on Win32. Stored as uintptr_t to keep
    // <winsock2.h> out of this public header (it conflicts with
    // <windows.h> include order if pulled in via a third TU).
    // The .cpp casts to/from SOCKET.
    uintptr_t               m_sock          = static_cast<uintptr_t>(~uintptr_t(0));

    // Peer address. Stored as raw bytes (sizeof(sockaddr_in) == 16 on
    // Win32) to avoid winsock2.h in this header. Host mode: written by
    // the recv thread on first packet, read by send(). Synchronised
    // via acquire/release on m_peer_known — this is NOT a data race:
    // the release-store on m_peer_known happens-after the memcpy into
    // m_peer_addr, and the acquire-load in send() happens-before the
    // memcpy out. The 16-byte addr is therefore guaranteed visible
    // whenever m_peer_known reads true. Audit clarification
    // 2026-05-12-c (F2-correctness, 72% conf).
    uint8_t                 m_peer_addr[16] = {};
    std::atomic<bool>       m_peer_known{false};

    RecvCallback            m_recv_cb;
    HANDLE                  m_recv_thread   = nullptr;
    std::atomic<bool>       m_stopping{false};

    std::atomic<uint64_t>   m_packets_sent{0};
    std::atomic<uint64_t>   m_packets_recvd{0};
    std::atomic<uint64_t>   m_send_errors{0};
    std::atomic<uint64_t>   m_bad_packets{0};
};

} // namespace mtr::coop::net
