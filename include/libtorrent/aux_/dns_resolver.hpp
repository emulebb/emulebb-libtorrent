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

#ifndef TORRENT_DNS_RESOLVER_HPP_INCLUDE
#define TORRENT_DNS_RESOLVER_HPP_INCLUDE

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "libtorrent/error_code.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/address.hpp"
#include "libtorrent/aux_/resolver_interface.hpp"

namespace libtorrent {
namespace aux {

// A minimal DNS-over-UDP resolver. Unlike the system resolver (getaddrinfo,
// used by aux::resolver), every query is sent from a UDP socket that we bind
// and egress-pin to a chosen local interface, so name lookups stay on the
// configured (e.g. VPN) interface instead of leaking out the default route /
// system resolver. It resolves A and AAAA records and follows the answer
// section as returned by the server (it does not chase CNAMEs itself).
struct TORRENT_EXTRA_EXPORT dns_resolver final : resolver_interface
{
	// dns_server is the upstream resolver to query. bind_address is the local
	// address to bind queries to (may be unspecified to let the OS pick).
	// if_index is the OS interface index used for IP_UNICAST_IF egress pinning
	// on Windows (0 = don't pin).
	dns_resolver(io_context& ios, udp::endpoint const& dns_server
		, address const& bind_address, int if_index);

	void async_resolve(std::string const& host, resolver_flags flags
		, callback_t h) override;
	void abort() override;
	void set_cache_timeout(seconds timeout) override;

private:
	struct query;

	void receive(std::shared_ptr<query> q);
	void complete(std::shared_ptr<query> q, error_code const& ec);

	struct dns_cache_entry
	{
		time_point last_seen;
		std::vector<address> addresses;
	};

	io_context& m_ios;
	udp::endpoint m_dns_server;
	address m_bind_address;
	int m_if_index;

	std::unordered_map<std::string, dns_cache_entry> m_cache;
	time_duration m_timeout;

	std::vector<std::shared_ptr<query>> m_active;
};

}
}

#endif
