#include "LinuxSocket.h"

#ifdef CAM_PLATFORM_LINUX

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
		m_Connection = -1;
	}

	LinuxSocket::~LinuxSocket()
	{
		Close();
	}
	
	bool LinuxSocket::Open(bool is_client, const std::string &ip, uint16 port)
	{
		Close();

		int32 opt = 1;

		// Creating socket file descriptor
		if ((m_Socket = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		{
			return false;
		}

		// Forcefully attaching socket to the port 8080
		if (setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
		{
			return false;
		}

		if (is_client)
		{
			struct sockaddr_in serv_addr;
			int32 status;

			serv_addr.sin_family = AF_INET;
			serv_addr.sin_port = htons(port);

			// Convert IPv4 and IPv6 addresses from text to binary form
			if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0)
			{
				return false;
			}

			if ((status = connect(m_Socket, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) < 0)
			{
				return false;
			}
		}

		return true;
	}
	
	void LinuxSocket::Close()
	{
		if (m_Connection != -1)
		{
			close(m_Connection);
			shutdown(m_Socket, SHUT_RDWR);
			m_Socket = -1;
			m_Connection = -1;
			return;
		}

		close(m_Socket);
		m_Socket = -1;
	}
	
	bool LinuxSocket::Bind(uint16 port)
	{
		struct sockaddr_in address;
		int32 addrlen = sizeof(address);
		
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
		address.sin_port = htons(port);

		// Forcefully attaching socket to the port
		if (bind(m_Socket, (struct sockaddr *)&address, sizeof(address)) < 0)
		{
			return false;
		}

		if (listen(m_Socket, 3) < 0)
		{
			return false;
		}

		if ((m_Connection = accept(m_Socket, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0)
		{
			return false;
		}

		return true;
	}
	
	int32 LinuxSocket::Recv(void *dst, int32 dst_bytes, addr_t *addr)
	{
		int32 handle = m_Connection == -1 ? m_Socket : m_Connection;
		struct sockaddr dest_addr;
		socklen_t addrLen = sizeof(dest_addr);

		int32 bytes_received = recvfrom(handle, dst, dst_bytes, 0, &dest_addr, &addrLen);
		struct sockaddr_in *dest_conn_info = (struct sockaddr_in*)GetInAddr(&dest_addr);

		addr->Host = dest_conn_info->sin_addr.s_addr;
		addr->Port = dest_conn_info->sin_port;
		return bytes_received;
	}
	
	int32 LinuxSocket::Send(void const *src, int32 src_bytes, addr_t addr)
	{
		int32 handle = m_Connection == -1 ? m_Socket : m_Connection;
		struct sockaddr_in dest_addr;

		memset(&dest_addr, 0, sizeof(dest_addr));
		dest_addr.sin_family = AF_INET;
		dest_addr.sin_addr.s_addr = addr.Host;
		dest_addr.sin_port = addr.Port;

		return sendto(handle, src, src_bytes, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	}

	int32 LinuxSocket::SendLarge(void const *src, int32 src_bytes, addr_t addr)
	{
		int32 send_pos = 0;
		int32 buffer_size = 256;
		int32 bytes_left = src_bytes;
		int32 n = -1;
		
		int32 handle = m_Connection == -1 ? m_Socket : m_Connection;
		struct sockaddr_in dest_addr;

		memset(&dest_addr, 0, sizeof(dest_addr));
		dest_addr.sin_family = AF_INET;
		dest_addr.sin_addr.s_addr = addr.Host;
		dest_addr.sin_port = addr.Port;

		while (send_pos < src_bytes)
		{
			int32 chunk_size = bytes_left > buffer_size ? buffer_size : bytes_left;
			n = sendto(handle, src + send_pos, chunk_size, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

			if (-1 == n)
			{
				break;
			}

			if (n != chunk_size)
			{
				// if less bytes were send, increase/decrease only by that amount
				chunk_size = n;
			}

			send_pos += chunk_size;
			bytes_left -= chunk_size;
		}

		return send_pos == src_bytes ? send_pos : -1;
	}

	int32 LinuxSocket::RecvLarge(void *dst, int32 dst_bytes, addr_t *addr)
	{
		assert(dst);
		assert(dst_bytes);
		assert(addr);

		int32 handle = m_Connection == -1 ? m_Socket : m_Connection;
		struct sockaddr dest_addr;
		socklen_t addrLen = sizeof(dest_addr);

		int32 buffer_size = 256;
		int32 read_pos = 0;
		int32 bytes_received = 0;
		
		while (bytes_received != dst_bytes)
		{
			if (read_pos >= dst_bytes)
			{
				break;
			}
	
			uint32 chunk_size = dst_bytes > buffer_size ? buffer_size : dst_bytes;
			bytes_received = recvfrom(handle, dst + read_pos, chunk_size, 0, &dest_addr, &addrLen);

			if (bytes_received == -1)
			{
				break;
			}
			
			if (chunk_size != bytes_received)
			{
				chunk_size = bytes_received;
			}

			read_pos += chunk_size;

			struct sockaddr_in *dest_conn_info = (struct sockaddr_in*)GetInAddr(&dest_addr);
			addr->Host = dest_conn_info->sin_addr.s_addr;
			addr->Port = dest_conn_info->sin_port;
		}

		return bytes_received == dst_bytes ? bytes_received : -1;
	}
	
	bool LinuxSocket::SetNonBlocking(bool enabled)
	{
		return fcntl(m_Socket, F_SETFL, SOCK_NONBLOCK) != -1;
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
			if (inet_pton(AF_INET, hbuf, (void*)(&addr4)) < 1)
			{
				free(reqs[0]);
				reqs[0] = NULL;
				return {};
			}

			addr.Host = addr4.sin_addr.s_addr;
			addr.Port = port;
		}

		free(reqs[0]);
		reqs[0] = NULL;
		return addr;
	}
}

#endif // CAM_PLATFORM_LINUX

