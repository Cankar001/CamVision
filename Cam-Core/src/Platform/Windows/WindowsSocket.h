#pragma once

#ifdef CAM_PLATFORM_WINDOWS

#include "Net/Socket.h"
#include <WinSock2.h>

namespace Core
{
	class WindowsSocket : public Socket
	{
	public:

		WindowsSocket();
		~WindowsSocket();

		virtual bool Open(bool is_client = false, const std::string &ip = "", uint16 port = 0) override;
		virtual void Close() override;

		virtual bool Bind(uint16 port) override;
		virtual bool BindLoopback(uint16 port) override;

		virtual int32 Recv(void *dst, int32 dst_bytes, addr_t *addr) override;
		virtual int32 Send(void const *src, int32 src_bytes, addr_t addr) override;

		virtual bool SetNonBlocking(bool enabled) override;
		virtual bool SetBufferSizes(int32 send_bytes, int32 recv_bytes) override;
		virtual addr_t Lookup(const std::string &host, uint16 port) override;

	private:

		SOCKET m_Socket;
		static constexpr SOCKET INVALID = INVALID_SOCKET;

		bool m_IsWsaInitialized;
	};
}

#endif // CAM_PLATFORM_WINDOWS

