/*

Copyright (c) 2016, 2020, Arvid Norberg
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

#ifndef TORRENT_BIND_TO_DEVICE_HPP_INCLUDED
#define TORRENT_BIND_TO_DEVICE_HPP_INCLUDED

#include "libtorrent/config.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/error_code.hpp"

#if TORRENT_USE_IFCONF || TORRENT_USE_NETLINK || TORRENT_USE_SYSCTL
#include <sys/socket.h> // for SO_BINDTODEVICE
#include <netinet/in.h>
#endif

namespace libtorrent { namespace aux {

#if defined SO_BINDTODEVICE

	struct bind_to_device
	{
		explicit bind_to_device(char const* device): m_value(device) {}
		template<class Protocol>
		int level(Protocol const&) const { return SOL_SOCKET; }
		template<class Protocol>
		int name(Protocol const&) const { return SO_BINDTODEVICE; }
		template<class Protocol>
		char const* data(Protocol const&) const { return m_value; }
		template<class Protocol>
		size_t size(Protocol const&) const { return strlen(m_value) + 1; }
	private:
		char const* m_value;
	};

	template <typename T>
	void bind_device(T& sock, char const* device, error_code& ec)
	{
		sock.set_option(bind_to_device(device), ec);
	}

#define TORRENT_HAS_BINDTODEVICE 1

#elif defined IP_BOUND_IF

	struct bind_to_device
	{
		explicit bind_to_device(unsigned int idx): m_value(idx) {}
		template<class Protocol>
		int level(Protocol const&) const { return IPPROTO_IP; }
		template<class Protocol>
		int name(Protocol const&) const { return IP_BOUND_IF; }
		template<class Protocol>
		char const* data(Protocol const&) const { return reinterpret_cast<char const*>(&m_value); }
		template<class Protocol>
		size_t size(Protocol const&) const { return sizeof(m_value); }
	private:
		unsigned int m_value;
	};

	template <typename T>
	void bind_device(T& sock, char const* device, error_code& ec)
	{
		unsigned int const if_idx = if_nametoindex(device);
		if (if_idx == 0)
		{
			ec.assign(errno, system_category());
			return;
		}
		sock.set_option(bind_to_device(if_idx), ec);
	}

#define TORRENT_HAS_BINDTODEVICE 1

#elif defined IP_FORCE_OUT_IFP

	struct bind_to_device
	{
		explicit bind_to_device(char const* device): m_value(device) {}
		template<class Protocol>
		int level(Protocol const&) const { return SOL_SOCKET; }
		template<class Protocol>
		int name(Protocol const&) const { return IP_FORCE_OUT_IFP; }
		template<class Protocol>
		char const* data(Protocol const&) const { return m_value; }
		template<class Protocol>
		size_t size(Protocol const&) const { return strlen(m_value) + 1; }
	private:
		char const* m_value;
	};

	template <typename T>
	void bind_device(T& sock, char const* device, error_code& ec)
	{
		sock.set_option(bind_to_device(device), ec);
	}

#define TORRENT_HAS_BINDTODEVICE 1

#else

#define TORRENT_HAS_BINDTODEVICE 0

#endif

#ifdef TORRENT_WINDOWS

	// Pin a socket's outgoing interface by OS interface index using
	// IP_UNICAST_IF / IPV6_UNICAST_IF. On Windows, bind()ing a socket to a
	// source address does NOT determine the egress interface -- the weak host
	// model lets the routing table pick a NIC, so a VPN-bound socket can still
	// send out another interface. IP_UNICAST_IF overrides the route lookup and
	// forces the interface. Quirk: the IPv4 option takes the index in *network*
	// byte order, the IPv6 option in *host* byte order.
	struct unicast_interface_v4
	{
		explicit unicast_interface_v4(unsigned int idx) : m_value(::htonl(idx)) {}
		template<class Protocol> int level(Protocol const&) const { return IPPROTO_IP; }
		template<class Protocol> int name(Protocol const&) const { return IP_UNICAST_IF; }
		template<class Protocol> char const* data(Protocol const&) const { return reinterpret_cast<char const*>(&m_value); }
		template<class Protocol> size_t size(Protocol const&) const { return sizeof(m_value); }
	private:
		::DWORD m_value;
	};

	struct unicast_interface_v6
	{
		explicit unicast_interface_v6(unsigned int idx) : m_value(idx) {}
		template<class Protocol> int level(Protocol const&) const { return IPPROTO_IPV6; }
		template<class Protocol> int name(Protocol const&) const { return IPV6_UNICAST_IF; }
		template<class Protocol> char const* data(Protocol const&) const { return reinterpret_cast<char const*>(&m_value); }
		template<class Protocol> size_t size(Protocol const&) const { return sizeof(m_value); }
	private:
		::DWORD m_value;
	};

	// Best-effort egress-interface pin. A zero/negative index is treated as
	// "unknown" and left to the OS. Callers decide whether a set_option failure
	// is fatal (strict/vpn mode) or advisory.
	template <typename T>
	void bind_socket_to_interface_index(T& sock, int if_index, bool v4, error_code& ec)
	{
		if (if_index <= 0) return;
		if (v4)
			sock.set_option(unicast_interface_v4(static_cast<unsigned int>(if_index)), ec);
		else
			sock.set_option(unicast_interface_v6(static_cast<unsigned int>(if_index)), ec);
	}

#endif // TORRENT_WINDOWS

} }

#endif

