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

// Sends a single RFC-5389 STUN Binding Request to `server` and reports the
// reflexive (public, server-observed) address from the XOR-MAPPED-ADDRESS
// attribute of the response. The query socket is bound to `bind_address` and,
// on Windows, egress-pinned to `if_index` via IP_UNICAST_IF -- so the reflexive
// address reflects the *actual* egress interface (e.g. a VPN tunnel). Pass an
// unspecified bind_address (and if_index 0) for a default-route ("clear") probe.
//
// `handler` is invoked exactly once: with an empty error_code and the reflexive
// address on success, or with an error_code (and an unspecified address) on
// failure/timeout.
TORRENT_EXTRA_EXPORT void stun_probe(io_context& ios
	, udp::endpoint const& server
	, address const& bind_address
	, int if_index
	, std::function<void(error_code const&, address const&)> handler);

}
}

#endif
