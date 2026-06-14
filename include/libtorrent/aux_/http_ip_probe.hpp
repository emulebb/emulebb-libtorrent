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

#ifndef TORRENT_HTTP_IP_PROBE_HPP_INCLUDE
#define TORRENT_HTTP_IP_PROBE_HPP_INCLUDE

#include <functional>
#include <string>

#include "libtorrent/error_code.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/address.hpp"

namespace libtorrent {
namespace aux {

// Performs a minimal plaintext HTTP GET against an IP-echo endpoint over a TCP
// socket bound and (on Windows) IP_UNICAST_IF-pinned to `bind_address` /
// `if_index`, and reports the public IP returned in the response body. This is
// the TCP-path analogue of stun_probe(): it verifies the egress IP that the
// *TCP* stack uses, which the binding/pin work makes the VPN interface.
//
// `server` is the already-resolved echo endpoint (use an IP literal to avoid a
// DNS dependency in the guard). `host` is the Host: header to send. `path` is
// the request path (e.g. "/"). Pass an unspecified bind_address for a
// default-route ("clear") probe. `handler` is invoked exactly once.
TORRENT_EXTRA_EXPORT void http_ip_probe(io_context& ios
	, tcp::endpoint const& server
	, std::string host
	, std::string path
	, address const& bind_address
	, int if_index
	, std::function<void(error_code const&, address const&)> handler);

}
}

#endif
