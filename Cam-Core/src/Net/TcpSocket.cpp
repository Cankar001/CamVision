#include "TcpSocket.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef CAM_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define CAM_POLL WSAPoll
#define CAM_CLOSE_SOCKET closesocket
#define CAM_SEND_FLAGS 0
typedef SOCKET SocketHandle;
static const SocketHandle INVALID_HANDLE = INVALID_SOCKET;

#else

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define CAM_POLL poll
#define CAM_CLOSE_SOCKET close
#define CAM_SEND_FLAGS MSG_NOSIGNAL
typedef int SocketHandle;
static const SocketHandle INVALID_HANDLE = -1;

#endif

namespace Core
{
	namespace
	{
		void StartNetwork()
		{
#ifdef CAM_PLATFORM_WINDOWS
			// Counted by Windows: the matching cleanup is not needed, the system cleans up, when the program ends.
			static std::once_flag once;
			std::call_once(once, []
			{
				WSADATA data;
				WSAStartup(MAKEWORD(2, 2), &data);
			});
#endif
		}

		SocketHandle ToHandle(intptr handle)
		{
			return (SocketHandle)handle;
		}

		std::string AddressText(const sockaddr_in &address, bool *loopback)
		{
			char text[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, (void *)&address.sin_addr, text, sizeof(text));
			*loopback = (ntohl(address.sin_addr.s_addr) >> 24) == 127;
			return std::string(text) + ":" + std::to_string(ntohs(address.sin_port));
		}

		// Waits until the socket can be read (or written). Returns 1 if yes, 0 after the timeout, -1 on an error.
		int WaitFor(SocketHandle handle, bool write, int timeoutMS)
		{
			pollfd descriptor = {};
			descriptor.fd = handle;
			descriptor.events = write ? POLLOUT : POLLIN;
			int result = CAM_POLL(&descriptor, 1, timeoutMS);
			if (result < 0)
			{
				return -1;
			}

			return result > 0 ? 1 : 0;
		}
	}

	TcpConnection::TcpConnection(intptr handle, const std::string &remote, bool loopback)
		: m_Handle(handle), m_Remote(remote), m_IsLoopback(loopback)
	{
		SocketHandle socket_handle = ToHandle(handle);
		int yes = 1;

		// Small messages (commands and answers) must not wait for more data.
		setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
		setsockopt(socket_handle, SOL_SOCKET, SO_KEEPALIVE, (const char *)&yes, sizeof(yes));
	}

	TcpConnection::~TcpConnection()
	{
		Close();
	}

	std::unique_ptr<TcpConnection> TcpConnection::Connect(const std::string &ip, uint16 port, int timeoutMS)
	{
		StartNetwork();

		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
		{
			return nullptr;
		}

		SocketHandle handle = socket(AF_INET, SOCK_STREAM, 0);
		if (handle == INVALID_HANDLE)
		{
			return nullptr;
		}

		// Connecting waits for the other side. The timeout is not applied to the connect call itself, a connection to this computer or the local
		// network is answered at once. (Used by tests and tools only.)
		(void)timeoutMS;
		if (connect(handle, (sockaddr *)&address, sizeof(address)) != 0)
		{
			CAM_CLOSE_SOCKET(handle);
			return nullptr;
		}

		bool loopback = false;
		std::string remote = AddressText(address, &loopback);
		return std::unique_ptr<TcpConnection>(new TcpConnection((intptr)handle, remote, loopback));
	}

	int32 TcpConnection::Receive(void *dst, int32 bytes, int timeoutMS)
	{
		SocketHandle handle = ToHandle(m_Handle);
		if ((intptr)handle == (intptr)INVALID_HANDLE)
		{
			return -1;
		}

		int waited = WaitFor(handle, false, timeoutMS);
		if (waited < 0)
		{
			return -1;
		}

		if (waited == 0)
		{
			return 0;
		}

		int received = (int)recv(handle, (char *)dst, bytes, 0);
		return received > 0 ? received : -1;
	}

	bool TcpConnection::Send(const void *data, uint32 bytes, int timeoutMS)
	{
		SocketHandle handle = ToHandle(m_Handle);
		const char *position = (const char *)data;
		uint32 left = bytes;
		while (left > 0)
		{
			if ((intptr)handle == (intptr)INVALID_HANDLE || WaitFor(handle, true, timeoutMS) != 1)
			{
				return false;
			}

			int sent = (int)send(handle, position, (int)std::min<uint32>(left, 1 << 20), CAM_SEND_FLAGS);
			if (sent <= 0)
			{
				return false;
			}

			position += sent;
			left -= (uint32)sent;
		}

		return true;
	}

	void TcpConnection::Close()
	{
		SocketHandle handle = ToHandle(m_Handle);
		if ((intptr)handle != (intptr)INVALID_HANDLE)
		{
			m_Handle = (intptr)INVALID_HANDLE;
			shutdown(handle, 2);
			CAM_CLOSE_SOCKET(handle);
		}
	}

	bool TcpConnection::IsOpen() const
	{
		return (intptr)ToHandle(m_Handle) != (intptr)INVALID_HANDLE;
	}

	TcpListener::TcpListener()
		: m_Handle((intptr)INVALID_HANDLE)
	{
	}

	TcpListener::~TcpListener()
	{
		Close();
	}

	bool TcpListener::Listen(const std::string &bindAddress, uint16 port)
	{
		StartNetwork();
		Close();

		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		if (inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1)
		{
			return false;
		}

		SocketHandle handle = socket(AF_INET, SOCK_STREAM, 0);
		if (handle == INVALID_HANDLE)
		{
			return false;
		}

		int yes = 1;
#ifdef CAM_PLATFORM_WINDOWS
		// On Windows, SO_REUSEADDR would let another program take over the port of a running one.
		setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes));
#else
		setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#endif

		if (bind(handle, (sockaddr *)&address, sizeof(address)) != 0 || listen(handle, 16) != 0)
		{
			CAM_CLOSE_SOCKET(handle);
			return false;
		}

		sockaddr_in bound = {};
#ifdef CAM_PLATFORM_WINDOWS
		int length = sizeof(bound);
#else
		socklen_t length = sizeof(bound);
#endif
		getsockname(handle, (sockaddr *)&bound, &length);
		m_Port = ntohs(bound.sin_port);
		m_Handle = (intptr)handle;
		return true;
	}

	std::unique_ptr<TcpConnection> TcpListener::Accept(int timeoutMS)
	{
		SocketHandle handle = ToHandle(m_Handle);
		if ((intptr)handle == (intptr)INVALID_HANDLE || WaitFor(handle, false, timeoutMS) != 1)
		{
			return nullptr;
		}

		sockaddr_in remote = {};
#ifdef CAM_PLATFORM_WINDOWS
		int length = sizeof(remote);
#else
		socklen_t length = sizeof(remote);
#endif
		SocketHandle accepted = accept(handle, (sockaddr *)&remote, &length);
		if (accepted == INVALID_HANDLE)
		{
			return nullptr;
		}

		bool loopback = false;
		std::string text = AddressText(remote, &loopback);
		return std::unique_ptr<TcpConnection>(new TcpConnection((intptr)accepted, text, loopback));
	}

	void TcpListener::Close()
	{
		SocketHandle handle = ToHandle(m_Handle);
		if ((intptr)handle != (intptr)INVALID_HANDLE)
		{
			m_Handle = (intptr)INVALID_HANDLE;
			CAM_CLOSE_SOCKET(handle);
		}
	}
}
