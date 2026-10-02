#pragma once

#ifdef CAM_PLATFORM_LINUX

#include "Net/Socket.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace Core
{
	class LinuxSocket : public Socket
	{
	public:

		LinuxSocket();
		~LinuxSocket();

		virtual bool Open(bool is_client = false, const std::string &ip = "", uint16 port = 0) override;
		virtual void Close() override;

		virtual bool Bind(uint16 port) override;

		virtual int32 Recv(void *dst, int32 dst_bytes, addr_t *addr) override;
		virtual int32 Send(void const *src, int32 src_bytes, addr_t addr) override;

		virtual bool SetNonBlocking(bool enabled) override;
		virtual bool SetBufferSizes(int32 send_bytes, int32 recv_bytes) override;
		virtual addr_t Lookup(const std::string &host, uint16 port) override;

	private:

		int32 m_Socket = -1;
	};
}

#endif // CAM_PLATFORM_LINUX

