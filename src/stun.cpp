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

#include "libtorrent/aux_/stun.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

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

	constexpr std::uint16_t stun_binding_request = 0x0001;
	constexpr std::uint16_t stun_binding_success = 0x0101;
	constexpr std::uint16_t attr_mapped_address = 0x0001;
	constexpr std::uint16_t attr_xor_mapped_address = 0x0020;
	constexpr std::uint32_t stun_magic_cookie = 0x2112A442;

	unsigned char char_cast(std::uint32_t v) { return static_cast<unsigned char>(v & 0xff); }

	void put_u16(char* p, std::uint16_t v)
	{
		p[0] = static_cast<char>((v >> 8) & 0xff);
		p[1] = static_cast<char>(v & 0xff);
	}

	std::uint16_t get_u16(char const* p)
	{
		return static_cast<std::uint16_t>(
			(static_cast<unsigned char>(p[0]) << 8) | static_cast<unsigned char>(p[1]));
	}

	std::uint32_t get_u32(char const* p)
	{
		return (std::uint32_t(static_cast<unsigned char>(p[0])) << 24)
			| (std::uint32_t(static_cast<unsigned char>(p[1])) << 16)
			| (std::uint32_t(static_cast<unsigned char>(p[2])) << 8)
			| std::uint32_t(static_cast<unsigned char>(p[3]));
	}

	// 20-byte header: type, length(0), magic cookie, 96-bit transaction id
	std::array<char, 20> build_request(std::array<char, 12> const& txid)
	{
		std::array<char, 20> req{};
		put_u16(req.data(), stun_binding_request);
		put_u16(req.data() + 2, 0);
		req[4] = char((stun_magic_cookie >> 24) & 0xff);
		req[5] = char((stun_magic_cookie >> 16) & 0xff);
		req[6] = char((stun_magic_cookie >> 8) & 0xff);
		req[7] = char(stun_magic_cookie & 0xff);
		std::memcpy(req.data() + 8, txid.data(), 12);
		return req;
	}

	// parses a STUN Binding Success response and extracts the reflexive address.
	error_code parse_response(char const* buf, int len
		, std::array<char, 12> const& txid, address& out)
	{
		auto const bad = error_code(boost::system::errc::bad_message, generic_category());
		if (len < 20) return bad;
		if (get_u16(buf) != stun_binding_success) return bad;
		if (get_u32(buf + 4) != stun_magic_cookie) return bad;
		if (std::memcmp(buf + 8, txid.data(), 12) != 0) return bad;

		int const msg_len = get_u16(buf + 2);
		int const total = std::min(len, 20 + msg_len);
		int pos = 20;
		while (pos + 4 <= total)
		{
			std::uint16_t const type = get_u16(buf + pos);
			int const alen = get_u16(buf + pos + 2);
			int const vpos = pos + 4;
			if (vpos + alen > total) break;

			if ((type == attr_xor_mapped_address || type == attr_mapped_address)
				&& alen >= 8)
			{
				// reserved(1) family(1) port(2) address(4|16)
				int const family = static_cast<unsigned char>(buf[vpos + 1]);
				bool const xor_form = (type == attr_xor_mapped_address);

				if (family == 0x01 && alen >= 8) // IPv4
				{
					std::uint32_t addr = get_u32(buf + vpos + 4);
					if (xor_form) addr ^= stun_magic_cookie;
					address_v4::bytes_type b{{
						char_cast(addr >> 24), char_cast(addr >> 16)
						, char_cast(addr >> 8), char_cast(addr)}};
					out = address_v4(b);
					return {};
				}
				if (family == 0x02 && alen >= 20) // IPv6
				{
					std::array<unsigned char, 16> a{};
					std::memcpy(a.data(), buf + vpos + 4, 16);
					if (xor_form)
					{
						unsigned char key[16];
						key[0] = (stun_magic_cookie >> 24) & 0xff;
						key[1] = (stun_magic_cookie >> 16) & 0xff;
						key[2] = (stun_magic_cookie >> 8) & 0xff;
						key[3] = stun_magic_cookie & 0xff;
						std::memcpy(key + 4, txid.data(), 12);
						for (int i = 0; i < 16; ++i) a[i] ^= key[i];
					}
					address_v6::bytes_type b;
					std::memcpy(b.data(), a.data(), 16);
					out = address_v6(b);
					return {};
				}
			}
			// attributes are padded to a 4-byte boundary
			pos = vpos + ((alen + 3) & ~3);
		}
		return error_code(boost::system::errc::no_message, generic_category());
	}

	constexpr int max_stun_response = 1500;

	struct probe_state
	{
		explicit probe_state(io_context& ios) : sock(ios), timer(ios) {}
		udp::socket sock;
		deadline_timer timer;
		udp::endpoint sender;
		std::array<char, 12> txid{};
		std::array<char, max_stun_response> recv_buf{};
		std::function<void(error_code const&, address const&)> handler;
		bool done = false;
	};

	void finish(std::shared_ptr<probe_state> st, error_code const& ec, address const& addr)
	{
		if (st->done) return;
		st->done = true;
		error_code ignore;
		st->timer.cancel();
		st->sock.close(ignore);
		if (st->handler) st->handler(ec, addr);
	}

} // anonymous namespace

void stun_probe(io_context& ios, udp::endpoint const& server
	, address const& bind_address, int if_index
	, std::function<void(error_code const&, address const&)> handler)
{
	auto st = std::make_shared<probe_state>(ios);
	st->handler = std::move(handler);

	for (int i = 0; i < 3; ++i)
	{
		std::uint32_t const r = libtorrent::random(0xffffffff);
		std::memcpy(st->txid.data() + i * 4, &r, 4);
	}

	error_code ec;
	st->sock.open(server.protocol(), ec);
	if (!ec && !bind_address.is_unspecified()
		&& bind_address.is_v4() == server.address().is_v4())
	{
#ifdef TORRENT_WINDOWS
		aux::bind_socket_to_interface_index(st->sock, if_index, bind_address.is_v4(), ec);
		ec.clear();
#else
		TORRENT_UNUSED(if_index);
#endif
		st->sock.bind(udp::endpoint(bind_address, 0), ec);
	}
	if (ec)
	{
		post(ios, [h = st->handler, ec]() { h(ec, address()); });
		return;
	}

	auto const req = std::make_shared<std::array<char, 20>>(build_request(st->txid));
	st->sock.async_send_to(boost::asio::buffer(*req), server
		, [req](error_code const&, std::size_t) {});

	st->sock.async_receive_from(boost::asio::buffer(st->recv_buf), st->sender
		, [st](error_code const& rec, std::size_t bytes)
	{
		if (st->done) return;
		if (rec) { finish(st, rec, address()); return; }
		address reflexive;
		error_code const pec = parse_response(st->recv_buf.data(), int(bytes), st->txid, reflexive);
		finish(st, pec, reflexive);
	});

	st->timer.expires_after(seconds(5));
	st->timer.async_wait([st](error_code const& tec)
	{
		if (tec) return; // cancelled
		finish(st, error_code(boost::system::errc::timed_out, generic_category()), address());
	});
}

}
}
