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

#include "libtorrent/aux_/dns_resolver.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "libtorrent/deadline_timer.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/aux_/time.hpp"
#include "libtorrent/aux_/bind_to_device.hpp"

#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/asio/buffer.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"

namespace libtorrent {
namespace aux {

namespace {

	constexpr std::uint16_t dns_type_a = 1;
	constexpr std::uint16_t dns_type_aaaa = 28;
	constexpr std::uint16_t dns_class_in = 1;

	void put_u16(std::vector<char>& v, std::uint16_t val)
	{
		v.push_back(static_cast<char>((val >> 8) & 0xff));
		v.push_back(static_cast<char>(val & 0xff));
	}

	std::uint16_t get_u16(char const* p)
	{
		return static_cast<std::uint16_t>(
			(static_cast<unsigned char>(p[0]) << 8) | static_cast<unsigned char>(p[1]));
	}

	// builds a standard recursive A/AAAA query for host
	std::vector<char> build_query(std::uint16_t id, std::string const& host
		, std::uint16_t qtype)
	{
		std::vector<char> q;
		q.reserve(host.size() + 18);
		put_u16(q, id);
		put_u16(q, 0x0100); // flags: RD (recursion desired)
		put_u16(q, 1);      // qdcount
		put_u16(q, 0);      // ancount
		put_u16(q, 0);      // nscount
		put_u16(q, 0);      // arcount

		// qname: sequence of length-prefixed labels, terminated by a zero byte
		std::size_t start = 0;
		while (start <= host.size())
		{
			std::size_t const dot = host.find('.', start);
			std::size_t const end = (dot == std::string::npos) ? host.size() : dot;
			std::size_t const len = end - start;
			if (len == 0 || len > 63)
			{
				// empty/oversized label: emit just the root and stop (the query
				// will simply not resolve, which the caller handles)
				if (start >= host.size()) break;
			}
			q.push_back(static_cast<char>(len & 0x3f));
			for (std::size_t i = start; i < end; ++i) q.push_back(host[i]);
			if (dot == std::string::npos) break;
			start = dot + 1;
		}
		q.push_back(0); // root label
		put_u16(q, qtype);
		put_u16(q, dns_class_in);
		return q;
	}

	// advances pos past a (possibly compressed) name. returns false on overflow.
	bool skip_name(char const* buf, int len, int& pos)
	{
		int guard = 0;
		while (pos < len)
		{
			if (++guard > len) return false;
			auto const b = static_cast<unsigned char>(buf[pos]);
			if ((b & 0xc0) == 0xc0)
			{
				// compression pointer: two bytes, name ends here
				pos += 2;
				return pos <= len;
			}
			if (b == 0) { pos += 1; return true; }
			pos += 1 + int(b);
		}
		return false;
	}

	// parses a DNS response, appending any A/AAAA records to out. returns an
	// error_code on a malformed packet or a server failure rcode.
	error_code parse_response(char const* buf, int len, std::vector<address>& out)
	{
		if (len < 12) return error_code(boost::system::errc::bad_message, generic_category());

		int const rcode = static_cast<unsigned char>(buf[3]) & 0x0f;
		if (rcode != 0)
			return error_code(boost::system::errc::host_unreachable, generic_category());

		int const qdcount = get_u16(buf + 4);
		int const ancount = get_u16(buf + 6);

		int pos = 12;
		for (int i = 0; i < qdcount; ++i)
		{
			if (!skip_name(buf, len, pos)) return error_code(boost::system::errc::bad_message, generic_category());
			pos += 4; // qtype + qclass
			if (pos > len) return error_code(boost::system::errc::bad_message, generic_category());
		}

		for (int i = 0; i < ancount; ++i)
		{
			if (!skip_name(buf, len, pos)) break;
			if (pos + 10 > len) break;
			std::uint16_t const type = get_u16(buf + pos);
			std::uint16_t const rdlength = get_u16(buf + pos + 8);
			pos += 10;
			if (pos + int(rdlength) > len) break;

			if (type == dns_type_a && rdlength == 4)
			{
				address_v4::bytes_type b;
				std::memcpy(b.data(), buf + pos, 4);
				out.push_back(address_v4(b));
			}
			else if (type == dns_type_aaaa && rdlength == 16)
			{
				address_v6::bytes_type b;
				std::memcpy(b.data(), buf + pos, 16);
				out.push_back(address_v6(b));
			}
			pos += int(rdlength);
		}
		return {};
	}

	constexpr int max_dns_response = 1500;
} // anonymous namespace

struct dns_resolver::query
{
	explicit query(io_context& ios) : sock(ios), timer(ios) {}

	udp::socket sock;
	deadline_timer timer;
	udp::endpoint sender;
	std::array<char, max_dns_response> recv_buf{};
	std::string host;
	std::vector<address> addresses;
	resolver_interface::callback_t handler;
	int pending = 0;   // outstanding responses (A + AAAA)
	bool done = false;
};

dns_resolver::dns_resolver(io_context& ios, udp::endpoint const& dns_server
	, address const& bind_address, int if_index)
	: m_ios(ios)
	, m_dns_server(dns_server)
	, m_bind_address(bind_address)
	, m_if_index(if_index)
	, m_timeout(seconds(1200))
{}

void dns_resolver::set_cache_timeout(seconds timeout)
{
	m_timeout = timeout;
}

void dns_resolver::abort()
{
	for (auto& q : m_active)
	{
		error_code ignore;
		q->timer.cancel();
		q->sock.close(ignore);
	}
	m_active.clear();
}

void dns_resolver::async_resolve(std::string const& host, resolver_flags const flags
	, callback_t h)
{
	// a literal IP needs no lookup
	{
		error_code pec;
		address const literal = make_address(host, pec);
		if (!pec)
		{
			std::vector<address> v{literal};
			post(m_ios, [h, v]() { h(error_code(), v); });
			return;
		}
	}

	auto const ci = m_cache.find(host);
	if (ci != m_cache.end()
		&& ((flags & resolver_interface::cache_only)
			|| (aux::time_now() - ci->second.last_seen) < m_timeout))
	{
		std::vector<address> const v = ci->second.addresses;
		post(m_ios, [h, v]() { h(error_code(), v); });
		return;
	}

	if (flags & resolver_interface::cache_only)
	{
		post(m_ios, [h]() { h(error_code(boost::system::errc::host_unreachable, generic_category()), {}); });
		return;
	}

	auto q = std::make_shared<query>(m_ios);
	q->host = host;
	q->handler = std::move(h);

	error_code ec;
	q->sock.open(m_dns_server.protocol(), ec);
	if (!ec && !m_bind_address.is_unspecified()
		&& m_bind_address.is_v4() == m_dns_server.address().is_v4())
	{
#ifdef TORRENT_WINDOWS
		aux::bind_socket_to_interface_index(q->sock, m_if_index, m_bind_address.is_v4(), ec);
		ec.clear();
#endif
		q->sock.bind(udp::endpoint(m_bind_address, 0), ec);
	}
	if (ec)
	{
		auto handler = q->handler;
		post(m_ios, [handler, ec]() { handler(ec, {}); });
		return;
	}

	m_active.push_back(q);

	std::uint16_t const id_a = std::uint16_t(libtorrent::random(0xffff));
	std::uint16_t const id_aaaa = std::uint16_t(libtorrent::random(0xffff));
	auto const query_a = std::make_shared<std::vector<char>>(build_query(id_a, host, dns_type_a));
	auto const query_aaaa = std::make_shared<std::vector<char>>(build_query(id_aaaa, host, dns_type_aaaa));
	q->pending = 2;

	for (auto const& msg : {query_a, query_aaaa})
	{
		q->sock.async_send_to(boost::asio::buffer(*msg), m_dns_server
			, [msg](error_code const&, std::size_t) {});
	}

	receive(q);

	q->timer.expires_after(seconds(5));
	q->timer.async_wait([this, q](error_code const& tec)
	{
		if (tec) return; // cancelled
		// timeout: complete with whatever we have (success if any address)
		complete(q, q->addresses.empty()
			? error_code(boost::system::errc::timed_out, generic_category())
			: error_code());
	});
}

void dns_resolver::receive(std::shared_ptr<query> q)
{
	q->sock.async_receive_from(boost::asio::buffer(q->recv_buf), q->sender
		, [this, q](error_code const& ec, std::size_t bytes)
	{
		if (q->done) return;
		if (ec)
		{
			// a recv error with nothing gathered fails; otherwise rely on timer
			if (q->addresses.empty() && ec != boost::asio::error::operation_aborted)
				complete(q, ec);
			return;
		}

		std::vector<address> parsed;
		parse_response(q->recv_buf.data(), int(bytes), parsed);
		for (auto const& a : parsed) q->addresses.push_back(a);

		if (--q->pending <= 0)
		{
			complete(q, q->addresses.empty()
				? error_code(boost::system::errc::host_unreachable, generic_category())
				: error_code());
		}
		else
		{
			receive(q);
		}
	});
}

void dns_resolver::complete(std::shared_ptr<query> q, error_code const& ec)
{
	if (q->done) return;
	q->done = true;

	error_code ignore;
	q->timer.cancel();
	q->sock.close(ignore);

	if (!ec && !q->addresses.empty())
	{
		// de-duplicate while preserving order
		std::vector<address> uniq;
		for (auto const& a : q->addresses)
			if (std::find(uniq.begin(), uniq.end(), a) == uniq.end())
				uniq.push_back(a);
		q->addresses.swap(uniq);

		m_cache[q->host] = dns_cache_entry{aux::time_now(), q->addresses};
	}

	auto const it = std::find(m_active.begin(), m_active.end(), q);
	if (it != m_active.end()) m_active.erase(it);

	if (q->handler) q->handler(ec, q->addresses);
}

}
}
