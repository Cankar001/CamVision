#pragma once

#include "Core/Core.h"
#include "TcpSocket.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Core
{
	/// <summary>
	/// The WebSocket protocol (RFC 6455), without a library: the handshake, and the frames. Only what a server for text messages needs.
	/// </summary>
	namespace websocket
	{
		enum Opcode : uint8
		{
			CONTINUATION = 0x0,
			TEXT = 0x1,
			BINARY = 0x2,
			CLOSE = 0x8,
			PING = 0x9,
			PONG = 0xA
		};

		// The codes of a close message.
		constexpr uint16 CLOSE_NORMAL = 1000;
		constexpr uint16 CLOSE_GOING_AWAY = 1001;
		constexpr uint16 CLOSE_PROTOCOL_ERROR = 1002;
		constexpr uint16 CLOSE_UNSUPPORTED = 1003;
		constexpr uint16 CLOSE_POLICY = 1008;
		constexpr uint16 CLOSE_TOO_BIG = 1009;

		/// <summary>
		/// The answer key of the handshake for the key of the client (SHA-1 and Base64, as the protocol demands).
		/// </summary>
		std::string AcceptKey(const std::string &clientKey);

		std::string Base64Encode(const Byte *data, uint32 bytes);

		struct HandshakeRequest
		{
			std::string Path;
			std::string Key;
			std::map<std::string, std::string> Headers;		// the names in lower case
		};

		/// <summary>
		/// Reads the HTTP request of a client, which wants to start a WebSocket. Returns false, with the reason, if it is not one.
		/// </summary>
		bool ParseHandshake(const std::string &request, HandshakeRequest *out, std::string *error);

		/// <summary>
		/// The answer (HTTP, "101 Switching Protocols") to a valid handshake.
		/// </summary>
		std::string HandshakeResponse(const HandshakeRequest &request);

		/// <summary>
		/// One frame of the stream. A server sends frames without a mask, a client has to mask its frames (the tests do).
		/// </summary>
		std::vector<Byte> EncodeFrame(uint8 opcode, const void *payload, uint64 bytes, bool mask, bool fin = true);

		std::vector<Byte> EncodeClose(uint16 code, const std::string &reason = "");

		struct Frame
		{
			bool Fin = true;
			uint8 Opcode = 0;
			std::vector<Byte> Payload;
		};

		/// <summary>
		/// Cuts the frames out of the bytes, which arrive in pieces. Frames of a client must be masked (a server refuses unmasked ones, and frames which are
		/// too large).
		/// </summary>
		class FrameParser
		{
		public:

			explicit FrameParser(uint64 maxPayloadBytes = 1 << 20, bool requireMask = true)
				: m_MaxPayload(maxPayloadBytes), m_RequireMask(requireMask)
			{
			}

			void Feed(const void *data, size_t bytes);

			enum class Result { NeedMore, Frame, Error };

			/// <summary>
			/// Takes the next complete frame out. On an error, the close code says what is wrong (the connection must be closed).
			/// </summary>
			Result Next(Frame *out, uint16 *closeCode);

		private:

			std::vector<Byte> m_Buffer;
			uint64 m_MaxPayload;
			bool m_RequireMask;
		};
	}

	/// <summary>
	/// What the handler of the server knows about a connection.
	/// </summary>
	struct WebSocketSession
	{
		uint64 Id = 0;
		std::string Remote;
		bool IsLoopback = false;

		/// <summary>
		/// Set by the handler, when the client has proven who it is. A client, which does not do that in time, is disconnected.
		/// </summary>
		bool Authenticated = false;
	};

	struct WebSocketServerConfig
	{
		/// <summary>
		/// "127.0.0.1": only programs on this computer. "0.0.0.0": the whole network.
		/// </summary>
		std::string BindAddress = "127.0.0.1";

		/// <summary>
		/// 0 takes a free port (see WebSocketServer::Port()).
		/// </summary>
		uint16 Port = 0;

		uint32 MaxConnections = 8;
		uint32 MaxMessageBytes = 64 * 1024;
		int HandshakeTimeoutMS = 5000;

		/// <summary>
		/// A client, which has not authenticated within this time, is disconnected.
		/// </summary>
		int AuthenticationTimeoutMS = 10000;
	};

	/// <summary>
	/// A server for WebSocket connections with text messages. Every connection has a thread, which reads the messages, gives them to the handler (one at a
	/// time per connection), and sends its answer.
	/// </summary>
	class WebSocketServer
	{
	public:

		/// <summary>
		/// Called for every text message. The answer is sent back (nothing, if it is empty). Setting closeConnection ends the connection after the answer.
		/// </summary>
		using MessageHandler = std::function<void(WebSocketSession &session, const std::string &message, std::string *answer, bool *closeConnection)>;

		/// <summary>
		/// Called when a client is connected (after the handshake) and when it is gone.
		/// </summary>
		using SessionHandler = std::function<void(WebSocketSession &session)>;

		WebSocketServer(const WebSocketServerConfig &config, MessageHandler onMessage);
		~WebSocketServer();

		WebSocketServer(const WebSocketServer &) = delete;
		WebSocketServer &operator=(const WebSocketServer &) = delete;

		void SetSessionHandlers(SessionHandler onOpen, SessionHandler onClose);

		/// <summary>
		/// Starts to listen, and to accept connections on a thread of its own. Returns false, if the port could not be opened.
		/// </summary>
		bool Start();

		/// <summary>
		/// Disconnects everybody and stops all threads.
		/// </summary>
		void Stop();

		uint16 Port() const { return m_Listener.Port(); }

		uint32 ConnectionCount() const;

		/// <summary>
		/// Sends a text message to all clients, which are authenticated (for events).
		/// </summary>
		void Broadcast(const std::string &message);

	private:

		struct Connection;

		void AcceptLoop();
		void Serve(std::shared_ptr<Connection> connection);
		bool Handshake(Connection &connection, std::string *leftover);
		bool SendFrame(Connection &connection, const std::vector<Byte> &frame);

		WebSocketServerConfig m_Config;
		MessageHandler m_OnMessage;
		SessionHandler m_OnOpen;
		SessionHandler m_OnClose;

		TcpListener m_Listener;
		std::thread m_AcceptThread;
		std::atomic<bool> m_Running{ false };

		mutable std::mutex m_Mutex;
		std::vector<std::shared_ptr<Connection>> m_Connections;
		uint64 m_NextId = 1;
	};
}
