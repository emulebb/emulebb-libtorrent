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

#include "libtorrent/aux_/http_ip_probe.hpp"

#include <array>
#include <memory>
#include <utility>

#include "libtorrent/deadline_timer.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/aux_/time.hpp"
#include "libtorrent/aux_/bind_to_device.hpp"

#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"

namespace libtorrent {
namespace aux {

namespace {

	// extracts the first IP-address-looking token from an HTTP response body.
	address parse_body_address(std::string const& data, error_code& ec)
	{
		// skip headers
		auto const hdr_end = data.find("\r\n\r\n");
		std::string body = (hdr_end == std::string::npos) ? data : data.substr(hdr_end + 4);

		// trim and try each whitespace-delimited token
		std::size_t pos = 0;
		while (pos < body.size())
		{
			while (pos < body.size()
				&& (body[pos] == ' ' || body[pos] == '\r' || body[pos] == '\n'
					|| body[pos] == '\t'))
				++pos;
			std::size_t end = pos;
			while (end < body.size()
				&& body[end] != ' ' && body[end] != '\r' && body[end] != '\n'
				&& body[end] != '\t')
				++end;
			if (end > pos)
			{
				error_code pec;
				address const a = make_address(body.substr(pos, end - pos), pec);
				if (!pec) { ec.clear(); return a; }
			}
			pos = end;
		}
		ec = error_code(boost::system::errc::bad_message, generic_category());
		return address();
	}

	constexpr int max_http_response = 8192;

	struct probe_state
	{
		explicit probe_state(io_context& ios) : sock(ios), timer(ios) {}
		tcp::socket sock;
		deadline_timer timer;
		std::string request;
		std::array<char, 2048> buf{};
		std::string response;
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

	void do_read(std::shared_ptr<probe_state> st)
	{
		st->sock.async_read_some(boost::asio::buffer(st->buf)
			, [st](error_code const& ec, std::size_t bytes)
		{
			if (st->done) return;
			if (bytes > 0 && st->response.size() < std::size_t(max_http_response))
				st->response.append(st->buf.data(), bytes);

			if (ec)
			{
				// EOF (connection closed) is the normal end of a bottled response
				if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset)
				{
					error_code pec;
					address const a = parse_body_address(st->response, pec);
					finish(st, pec, a);
				}
				else
				{
					finish(st, ec, address());
				}
				return;
			}
			do_read(st);
		});
	}

} // anonymous namespace

void http_ip_probe(io_context& ios, tcp::endpoint const& server
	, std::string host, std::string path
	, address const& bind_address, int if_index
	, std::function<void(error_code const&, address const&)> handler)
{
	auto st = std::make_shared<probe_state>(ios);
	st->handler = std::move(handler);
	if (path.empty()) path = "/";

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
		st->sock.bind(tcp::endpoint(bind_address, 0), ec);
	}
	if (ec)
	{
		post(ios, [h = st->handler, ec]() { h(ec, address()); });
		return;
	}

	st->request = "GET " + path + " HTTP/1.1\r\nHost: " + host
		+ "\r\nUser-Agent: libtorrent-vpn-guard\r\nAccept: text/plain\r\n"
		+ "Connection: close\r\n\r\n";

	st->sock.async_connect(server, [st](error_code const& cec)
	{
		if (st->done) return;
		if (cec) { finish(st, cec, address()); return; }
		auto const req = std::make_shared<std::string>(st->request);
		st->sock.async_send(boost::asio::buffer(*req)
			, [st, req](error_code const& sec, std::size_t)
		{
			if (st->done) return;
			if (sec) { finish(st, sec, address()); return; }
			do_read(st);
		});
	});

	st->timer.expires_after(seconds(5));
	st->timer.async_wait([st](error_code const& tec)
	{
		if (tec) return;
		finish(st, error_code(boost::system::errc::timed_out, generic_category()), address());
	});
}

}
}
