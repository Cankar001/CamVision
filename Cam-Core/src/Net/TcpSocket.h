#pragma once

#include "Core/Core.h"

#include <memory>
#include <string>

namespace Core
{
	/// <summary>
	/// A connection over TCP (a stream of bytes). Used for the WebSocket of the server. The functions wait with a timeout, so a thread can look at its stop
	/// flag from time to time. Sending and receiving can be done by different threads at the same time.
	/// </summary>
	class TcpConnection
	{
	public:

		~TcpConnection();

		TcpConnection(const TcpConnection &) = delete;
		TcpConnection &operator=(const TcpConnection &) = delete;

		/// <summary>
		/// Connects to a server (used by the tests, and tools).
		/// </summary>
		static std::unique_ptr<TcpConnection> Connect(const std::string &ip, uint16 port, int timeoutMS);

		/// <summary>
		/// Waits up to the timeout for data.
		/// </summary>
		/// <returns>The number of bytes, 0 if nothing arrived in time, -1 if the connection is closed or broken.</returns>
		int32 Receive(void *dst, int32 bytes, int timeoutMS);

		/// <summary>
		/// Sends everything (waits, if the other side is slow, up to the timeout for every part). Returns false, if that did not work.
		/// </summary>
		bool Send(const void *data, uint32 bytes, int timeoutMS = 5000);

		void Close();

		bool IsOpen() const;

		/// <summary>
		/// The address of the other side, "127.0.0.1:51234" (for logs).
		/// </summary>
		const std::string &RemoteAddress() const { return m_Remote; }

		/// <summary>
		/// True, if the other side is on this computer (127.x.x.x).
		/// </summary>
		bool IsLoopback() const { return m_IsLoopback; }

	private:

		friend class TcpListener;
		TcpConnection(intptr handle, const std::string &remote, bool loopback);

		intptr m_Handle;
		std::string m_Remote;
		bool m_IsLoopback;
	};

	/// <summary>
	/// Waits for connections on a port.
	/// </summary>
	class TcpListener
	{
	public:

		TcpListener();
		~TcpListener();

		TcpListener(const TcpListener &) = delete;
		TcpListener &operator=(const TcpListener &) = delete;

		/// <summary>
		/// Starts to listen. "127.0.0.1" only takes connections from this computer, "0.0.0.0" from the whole network. Port 0 takes a free port (see Port()).
		/// </summary>
		bool Listen(const std::string &bindAddress, uint16 port);

		/// <summary>
		/// Waits up to the timeout for a connection. Returns nullptr, if there was none.
		/// </summary>
		std::unique_ptr<TcpConnection> Accept(int timeoutMS);

		void Close();

		/// <summary>
		/// The port, on which it listens.
		/// </summary>
		uint16 Port() const { return m_Port; }

	private:

		intptr m_Handle;
		uint16 m_Port = 0;
	};
}
