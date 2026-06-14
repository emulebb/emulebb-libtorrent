/*

Copyright (c) 2026, the eMuleBB / qBittorrentBB project
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef TORRENT_STUN_HPP_INCLUDE
#define TORRENT_STUN_HPP_INCLUDE

#include <functional>

#include "libtorrent/error_code.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/address.hpp"

namespace libtorrent {
namespace aux {

// Races RFC-5389 STUN Binding Requests against a built-in set of public STUN
// servers concurrently (resolving each hostname asynchronously) and reports the
// reflexive (public, server-observed) address from the first valid
// XOR-MAPPED-ADDRESS / MAPPED-ADDRESS response. Resilience comes from the
// multi-server fan-out, not from per-socket retransmits (each server is queried
// once); when one server wins, the remaining in-flight probes are cancelled
// immediately. Every query socket is bound to `bind_address` and, on Windows,
// egress-pinned to `if_index` via IP_UNICAST_IF -- so the reflexive address
// reflects the *actual* egress interface (e.g. a VPN tunnel) -- and connect()ed
// to its server so the kernel drops datagrams from any other source (no spoofed
// responses). Each per-server probe (resolve + connect + send + recv) is bounded
// by a single timeout. Pass an unspecified bind_address (and if_index 0) for a
// default-route ("clear") probe.
//
// The built-in server list is kept in sync with the eMuleBB
// (StunProbeSeams::GetStunIpv4ProbeServers) and emulebb-rust (DEFAULT_STUN_SERVERS)
// probes.
//
// `handler` is invoked exactly once: with an empty error_code and the reflexive
// address on the first success, or with an error_code (and an unspecified
// address) once every server has failed/timed out.
TORRENT_EXTRA_EXPORT void stun_probe(io_context& ios
	, address const& bind_address
	, int if_index
	, std::function<void(error_code const&, address const&)> handler);

}
}

#endif
