#pragma once

#include "Core/Core.h"
#include <string>

namespace Core
{
	// IPV4 host port/pair.
	union addr_t {
		uint64 Value;

		struct {
			uint32 Host;
			uint32 Port;
		};
	};

	class Socket
	{
	public:

		virtual ~Socket() {}

		virtual bool Open(bool is_client = false, const std::string &ip = "", uint16 port = 0) = 0;
		virtual void Close() = 0;

		virtual bool Bind(uint16 port) = 0;

		/// <summary>
		/// Like Bind, but only reachable from the same computer (127.0.0.1). For control channels, which must not be reachable from the network.
		/// </summary>
		virtual bool BindLoopback(uint16 port) = 0;

		/// Returns the number of bytes received, 0 if nothing is available on a non-blocking socket and -1 on a socket error.
		virtual int32 Recv(void *dst, int32 dst_bytes, addr_t *addr) = 0;
		virtual int32 Send(void const *src, int32 src_bytes, addr_t addr) = 0;

		virtual bool SetNonBlocking(bool enabled) = 0;

		/// <summary>
		/// Sets the size of the kernel send and receive buffers. Large buffers help to absorb bursts of datagrams (e.g. all chunks of one frame).
		/// </summary>
		virtual bool SetBufferSizes(int32 send_bytes, int32 recv_bytes) = 0;
		virtual addr_t Lookup(const std::string &host, uint16 port) = 0;

		static Socket *Create();
	};
}

