#include "LinuxSocket.h"

#ifdef CAM_PLATFORM_LINUX

#include <cerrno>
#include <iostream>
#include <assert.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>

namespace Core
{
	// get sockaddr, IPv4 or IPv6:
	static void *GetInAddr(struct sockaddr *sa)
	{
    	if (sa->sa_family == AF_INET)
        	return &(((struct sockaddr_in*)sa)->sin_addr);
    	return &(((struct sockaddr_in6*)sa)->sin6_addr);
	}

	LinuxSocket::LinuxSocket()
	{
		m_Socket = -1;
	}

	LinuxSocket::~LinuxSocket()
	{
		Close();
	}
	
	bool LinuxSocket::Open(bool is_client, const std::string &ip, uint16 port)
	{
		Close();

		int32 opt = 1;

		// Creating UDP socket file descriptor
		if ((m_Socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		{
			return false;
		}

		if (setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
		{
			return false;
		}

		return true;
	}
	
	void LinuxSocket::Close()
	{
		if (m_Socket != -1)
		{
			close(m_Socket);
			m_Socket = -1;
		}
	}
	
	bool LinuxSocket::Bind(uint16 port)
	{
		struct sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
		address.sin_port = htons(port);

		return (bind(m_Socket, (struct sockaddr *)&address, sizeof(address)) == 0);
	}
	
	int32 LinuxSocket::Recv(void *dst, int32 dst_bytes, addr_t *addr)
	{
		int32 handle = m_Socket;
		struct sockaddr dest_addr = {};
		socklen_t addrLen = sizeof(dest_addr);

		int32 bytes_received = recvfrom(handle, dst, dst_bytes, 0, &dest_addr, &addrLen);
		if (bytes_received < 0)
		{
			// Nothing to read on a non-blocking socket is not an error.
			return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
		}
		struct sockaddr_in *dest_conn_info = (struct sockaddr_in*)GetInAddr(&dest_addr);

		addr->Host = dest_conn_info->sin_addr.s_addr;
		addr->Port = dest_conn_info->sin_port;
		return bytes_received;
	}
	
	int32 LinuxSocket::Send(void const *src, int32 src_bytes, addr_t addr)
	{
		int32 handle = m_Socket;
		struct sockaddr_in dest_addr;

		memset(&dest_addr, 0, sizeof(dest_addr));
		dest_addr.sin_family = AF_INET;
		dest_addr.sin_addr.s_addr = addr.Host;
		dest_addr.sin_port = addr.Port;

		return sendto(handle, src, src_bytes, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	}

	bool LinuxSocket::SetNonBlocking(bool enabled)
	{
		int32 flags = fcntl(m_Socket, F_GETFL, 0);
		if (flags == -1)
		{
			return false;
		}

		flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
		return fcntl(m_Socket, F_SETFL, flags) != -1;
	}

	bool LinuxSocket::SetBufferSizes(int32 send_bytes, int32 recv_bytes)
	{
		bool send_ok = (setsockopt(m_Socket, SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)) == 0);
		bool recv_ok = (setsockopt(m_Socket, SOL_SOCKET, SO_RCVBUF, &recv_bytes, sizeof(recv_bytes)) == 0);
		return send_ok && recv_ok;
	}
	
	addr_t LinuxSocket::Lookup(const std::string &host, uint16 port)
	{
		assert(host.size() > 0);
		struct gaicb *reqs[1];
		char hbuf[NI_MAXHOST];
		struct addrinfo *res;

		addr_t addr = {};

		reqs[0] = (gaicb*)malloc(sizeof(*reqs[0]));
		memset(reqs[0], 0, sizeof(*reqs[0]));
		reqs[0]->ar_name = host.c_str();

		int32 ret = getaddrinfo_a(GAI_WAIT, reqs, 1, NULL);
		if (ret != 0)
		{
			free(reqs[0]);
			reqs[0] = NULL;
			return {};
		}

		ret = gai_error(reqs[0]);
		if (ret == 0)
		{
			res = reqs[0]->ar_result;

			ret = getnameinfo(res->ai_addr, res->ai_addrlen, hbuf, sizeof(hbuf), NULL, 0, NI_NUMERICHOST);
			if (ret != 0)
			{
				free(reqs[0]);
				reqs[0] = NULL;
				return {};
			}

    		sockaddr_in addr4 = {};
			if (inet_pton(AF_INET, hbuf, (void*)(&addr4.sin_addr)) < 1)
			{
				free(reqs[0]);
				reqs[0] = NULL;
				return {};
			}

			addr.Host = addr4.sin_addr.s_addr;
			addr.Port = htons(port);
		}

		free(reqs[0]);
		reqs[0] = NULL;
		return addr;
	}
}

#endif // CAM_PLATFORM_LINUX

