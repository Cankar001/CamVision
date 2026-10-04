#include "WebSocket.h"

#include "Core/CryptoPrimitives.h"
#include "Core/Log.h"
#include "Core/Timer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace Core
{
	namespace websocket
	{
		namespace
		{
			std::string LowerCase(std::string text)
			{
				std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
				return text;
			}

			std::string Trim(const std::string &text)
			{
				size_t begin = text.find_first_not_of(" \t");
				if (begin == std::string::npos)
				{
					return "";
				}

				size_t end = text.find_last_not_of(" \t");
				return text.substr(begin, end - begin + 1);
			}

			// The header "Connection: keep-alive, Upgrade" is a list.
			bool ListContains(const std::string &list, const std::string &wanted)
			{
				std::istringstream stream(LowerCase(list));
				std::string item;
				while (std::getline(stream, item, ','))
				{
					if (Trim(item) == wanted)
					{
						return true;
					}
				}

				return false;
			}
		}

		std::string Base64Encode(const Byte *data, uint32 bytes)
		{
			static const char DIGITS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string text;
			for (uint32 i = 0; i < bytes; i += 3)
			{
				uint32 chunk = (uint32)data[i] << 16;
				if (i + 1 < bytes)
				{
					chunk |= (uint32)data[i + 1] << 8;
				}

				if (i + 2 < bytes)
				{
					chunk |= data[i + 2];
				}

				text.push_back(DIGITS[(chunk >> 18) & 63]);
				text.push_back(DIGITS[(chunk >> 12) & 63]);
				text.push_back(i + 1 < bytes ? DIGITS[(chunk >> 6) & 63] : '=');
				text.push_back(i + 2 < bytes ? DIGITS[chunk & 63] : '=');
			}

			return text;
		}

		std::string AcceptKey(const std::string &clientKey)
		{
			// The protocol demands exactly this: the key of the client and a fixed text, hashed with SHA-1.
			std::string input = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
			Byte hash[crypto::SHA1_BYTES];
			crypto::Sha1(input.data(), (uint32)input.size(), hash);
			return Base64Encode(hash, crypto::SHA1_BYTES);
		}

		bool ParseHandshake(const std::string &request, HandshakeRequest *out, std::string *error)
		{
			std::istringstream stream(request);
			std::string line;
			if (!std::getline(stream, line))
			{
				*error = "empty request";
				return false;
			}

			// "GET /path HTTP/1.1"
			std::istringstream first(line);
			std::string method, path, version;
			first >> method >> path >> version;
			if (method != "GET" || path.empty() || version.rfind("HTTP/1.", 0) != 0)
			{
				*error = "not a GET request of HTTP/1.1";
				return false;
			}

			HandshakeRequest result;
			result.Path = path;
			while (std::getline(stream, line))
			{
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				{
					line.pop_back();
				}

				if (line.empty())
				{
					break;
				}

				size_t colon = line.find(':');
				if (colon == std::string::npos)
				{
					*error = "a header line without ':'";
					return false;
				}

				result.Headers[LowerCase(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
			}

			auto upgrade = result.Headers.find("upgrade");
			auto connection = result.Headers.find("connection");
			auto key = result.Headers.find("sec-websocket-key");
			auto version_header = result.Headers.find("sec-websocket-version");
			if (upgrade == result.Headers.end() || LowerCase(upgrade->second) != "websocket" || connection == result.Headers.end() || !ListContains(connection->second, "upgrade"))
			{
				*error = "this is not a WebSocket request (Upgrade: websocket is missing)";
				return false;
			}

			if (key == result.Headers.end() || key->second.empty() || key->second.size() > 64)
			{
				*error = "Sec-WebSocket-Key is missing";
				return false;
			}

			if (version_header == result.Headers.end() || version_header->second != "13")
			{
				*error = "only version 13 of the WebSocket protocol is supported";
				return false;
			}

			result.Key = key->second;
			*out = result;
			return true;
		}

		std::string HandshakeResponse(const HandshakeRequest &request)
		{
			return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + AcceptKey(request.Key) + "\r\n\r\n";
		}

		std::vector<Byte> EncodeFrame(uint8 opcode, const void *payload, uint64 bytes, bool mask, bool fin)
		{
			std::vector<Byte> frame;
			frame.push_back((Byte)((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

			Byte mask_bit = mask ? 0x80 : 0x00;
			if (bytes < 126)
			{
				frame.push_back((Byte)(mask_bit | bytes));
			}
			else if (bytes <= 0xFFFF)
			{
				frame.push_back((Byte)(mask_bit | 126));
				frame.push_back((Byte)(bytes >> 8));
				frame.push_back((Byte)bytes);
			}
			else
			{
				frame.push_back((Byte)(mask_bit | 127));
				for (int i = 7; i >= 0; --i)
				{
					frame.push_back((Byte)(bytes >> (8 * i)));
				}
			}

			Byte key[4] = {};
			if (mask)
			{
				crypto::RandomBytes(key, 4);
				frame.insert(frame.end(), key, key + 4);
			}

			size_t start = frame.size();
			frame.resize(start + (size_t)bytes);
			if (bytes > 0)
			{
				memcpy(frame.data() + start, payload, (size_t)bytes);
			}

			if (mask)
			{
				for (size_t i = 0; i < (size_t)bytes; ++i)
				{
					frame[start + i] ^= key[i % 4];
				}
			}

			return frame;
		}

		std::vector<Byte> EncodeClose(uint16 code, const std::string &reason)
		{
			std::vector<Byte> payload;
			payload.push_back((Byte)(code >> 8));
			payload.push_back((Byte)code);
			payload.insert(payload.end(), reason.begin(), reason.begin() + std::min<size_t>(reason.size(), 100));
			return EncodeFrame(CLOSE, payload.data(), payload.size(), false);
		}

		void FrameParser::Feed(const void *data, size_t bytes)
		{
			const Byte *begin = (const Byte *)data;
			m_Buffer.insert(m_Buffer.end(), begin, begin + bytes);
		}

		FrameParser::Result FrameParser::Next(Frame *out, uint16 *closeCode)
		{
			if (m_Buffer.size() < 2)
			{
				return Result::NeedMore;
			}

			Byte first = m_Buffer[0];
			Byte second = m_Buffer[1];
			bool fin = (first & 0x80) != 0;
			uint8 opcode = first & 0x0F;
			bool masked = (second & 0x80) != 0;
			uint64 length = second & 0x7F;
			size_t position = 2;

			if ((first & 0x70) != 0)
			{
				// Reserved bits are for extensions, which were not negotiated.
				*closeCode = CLOSE_PROTOCOL_ERROR;
				return Result::Error;
			}

			if (length == 126)
			{
				if (m_Buffer.size() < position + 2)
				{
					return Result::NeedMore;
				}

				length = ((uint64)m_Buffer[2] << 8) | m_Buffer[3];
				position += 2;
			}
			else if (length == 127)
			{
				if (m_Buffer.size() < position + 8)
				{
					return Result::NeedMore;
				}

				length = 0;
				for (int i = 0; i < 8; ++i)
				{
					length = (length << 8) | m_Buffer[position + i];
				}

				position += 8;
			}

			// The checks, which need no payload, come first: a frame, which is too large, is refused before it is received.
			bool control = (opcode & 0x08) != 0;
			if (control && (length > 125 || !fin))
			{
				*closeCode = CLOSE_PROTOCOL_ERROR;
				return Result::Error;
			}

			if (opcode != CONTINUATION && opcode != TEXT && opcode != BINARY && opcode != CLOSE && opcode != PING && opcode != PONG)
			{
				*closeCode = CLOSE_PROTOCOL_ERROR;
				return Result::Error;
			}

			if (m_RequireMask && !masked)
			{
				*closeCode = CLOSE_PROTOCOL_ERROR;
				return Result::Error;
			}

			if (length > m_MaxPayload)
			{
				*closeCode = CLOSE_TOO_BIG;
				return Result::Error;
			}

			size_t mask_bytes = masked ? 4 : 0;
			if (m_Buffer.size() < position + mask_bytes + (size_t)length)
			{
				return Result::NeedMore;
			}

			Byte key[4] = {};
			if (masked)
			{
				memcpy(key, m_Buffer.data() + position, 4);
				position += 4;
			}

			out->Fin = fin;
			out->Opcode = opcode;
			out->Payload.assign(m_Buffer.begin() + position, m_Buffer.begin() + position + (size_t)length);
			if (masked)
			{
				for (size_t i = 0; i < out->Payload.size(); ++i)
				{
					out->Payload[i] ^= key[i % 4];
				}
			}

			m_Buffer.erase(m_Buffer.begin(), m_Buffer.begin() + position + (size_t)length);
			return Result::Frame;
		}
	}

	struct WebSocketServer::Connection
	{
		std::unique_ptr<TcpConnection> Tcp;
		WebSocketSession Session;
		std::thread Thread;
		std::atomic<bool> Done{ false };
		std::atomic<bool> Ready{ false };		// the handshake is done

		// Frames of one connection are sent by its own thread and by Broadcast, never mixed.
		std::mutex SendMutex;
	};

	WebSocketServer::WebSocketServer(const WebSocketServerConfig &config, MessageHandler onMessage)
		: m_Config(config), m_OnMessage(std::move(onMessage))
	{
	}

	WebSocketServer::~WebSocketServer()
	{
		Stop();
	}

	void WebSocketServer::SetSessionHandlers(SessionHandler onOpen, SessionHandler onClose)
	{
		m_OnOpen = std::move(onOpen);
		m_OnClose = std::move(onClose);
	}

	bool WebSocketServer::Start()
	{
		if (m_Running)
		{
			return true;
		}

		if (!m_Listener.Listen(m_Config.BindAddress, m_Config.Port))
		{
			return false;
		}

		m_Running = true;
		m_AcceptThread = std::thread(&WebSocketServer::AcceptLoop, this);
		return true;
	}

	void WebSocketServer::Stop()
	{
		m_Running = false;
		if (m_AcceptThread.joinable())
		{
			m_AcceptThread.join();
		}

		std::vector<std::shared_ptr<Connection>> connections;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			connections.swap(m_Connections);
		}

		// The connection threads look at m_Running, so they end within a moment.
		for (auto &connection : connections)
		{
			if (connection->Thread.joinable())
			{
				connection->Thread.join();
			}
		}

		m_Listener.Close();
	}

	uint32 WebSocketServer::ConnectionCount() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		uint32 count = 0;
		for (const auto &connection : m_Connections)
		{
			if (!connection->Done)
			{
				++count;
			}
		}

		return count;
	}

	void WebSocketServer::Broadcast(const std::string &message)
	{
		std::vector<Byte> frame = websocket::EncodeFrame(websocket::TEXT, message.data(), message.size(), false);
		std::vector<std::shared_ptr<Connection>> connections;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			connections = m_Connections;
		}

		for (auto &connection : connections)
		{
			if (connection->Ready && !connection->Done && connection->Session.Authenticated)
			{
				SendFrame(*connection, frame);
			}
		}
	}

	bool WebSocketServer::SendFrame(Connection &connection, const std::vector<Byte> &frame)
	{
		std::lock_guard<std::mutex> lock(connection.SendMutex);
		return connection.Tcp->Send(frame.data(), (uint32)frame.size());
	}

	void WebSocketServer::AcceptLoop()
	{
		while (m_Running)
		{
			std::unique_ptr<TcpConnection> tcp = m_Listener.Accept(200);

			// Connections, which are finished, are cleaned up.
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				for (auto it = m_Connections.begin(); it != m_Connections.end();)
				{
					if ((*it)->Done)
					{
						if ((*it)->Thread.joinable())
						{
							(*it)->Thread.join();
						}

						it = m_Connections.erase(it);
					}
					else
					{
						++it;
					}
				}
			}

			if (!tcp)
			{
				continue;
			}

			std::shared_ptr<Connection> connection = std::make_shared<Connection>();
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_Connections.size() >= m_Config.MaxConnections)
				{
					// Too many: the new one is refused.
					static const char BUSY[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
					tcp->Send(BUSY, (uint32)sizeof(BUSY) - 1, 500);

					// Closing with the request of the client unread would reset the connection, and the answer could get lost.
					char ignored[1024];
					tcp->Receive(ignored, sizeof(ignored), 100);
					continue;
				}

				connection->Session.Id = m_NextId++;
				connection->Session.Remote = tcp->RemoteAddress();
				connection->Session.IsLoopback = tcp->IsLoopback();
				connection->Tcp = std::move(tcp);
				m_Connections.push_back(connection);
			}

			connection->Thread = std::thread(&WebSocketServer::Serve, this, connection);
		}
	}

	bool WebSocketServer::Handshake(Connection &connection, std::string *leftover)
	{
		std::string request;
		int64 start = Core::QueryMS();
		char buffer[1024];
		while (request.find("\r\n\r\n") == std::string::npos)
		{
			if (!m_Running || Core::QueryMS() - start > m_Config.HandshakeTimeoutMS || request.size() > 8192)
			{
				return false;
			}

			int32 received = connection.Tcp->Receive(buffer, sizeof(buffer), 100);
			if (received < 0)
			{
				return false;
			}

			request.append(buffer, (size_t)received);
		}

		websocket::HandshakeRequest handshake;
		std::string error;
		if (!websocket::ParseHandshake(request.substr(0, request.find("\r\n\r\n") + 4), &handshake, &error))
		{
			static const char BAD[] = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nThis is a WebSocket server.";
			connection.Tcp->Send(BAD, (uint32)sizeof(BAD) - 1, 500);
			CAM_LOG_DEBUG("A request to the WebSocket from {0} was refused: {1}", connection.Session.Remote, error);
			return false;
		}

		// Bytes behind the request (a client, which does not wait for the answer) belong to the first frames.
		*leftover = request.substr(request.find("\r\n\r\n") + 4);

		std::string response = websocket::HandshakeResponse(handshake);
		return connection.Tcp->Send(response.data(), (uint32)response.size());
	}

	void WebSocketServer::Serve(std::shared_ptr<Connection> connection)
	{
		Connection &c = *connection;
		std::string leftover;
		if (Handshake(c, &leftover))
		{
			c.Ready = true;
			if (m_OnOpen)
			{
				m_OnOpen(c.Session);
			}

			websocket::FrameParser parser(m_Config.MaxMessageBytes, true);
			parser.Feed(leftover.data(), leftover.size());
			std::string message;			// a message of several frames
			bool in_message = false;
			int64 connected = Core::QueryMS();
			bool closing = false;
			char buffer[4096];

			while (m_Running && !closing)
			{
				if (!c.Session.Authenticated && Core::QueryMS() - connected > m_Config.AuthenticationTimeoutMS)
				{
					SendFrame(c, websocket::EncodeClose(websocket::CLOSE_POLICY, "authentication timeout"));
					break;
				}

				int32 received = c.Tcp->Receive(buffer, sizeof(buffer), 200);
				if (received < 0)
				{
					break;
				}

				if (received > 0)
				{
					parser.Feed(buffer, (size_t)received);
				}

				for (;;)
				{
					websocket::Frame frame;
					uint16 close_code = 0;
					websocket::FrameParser::Result result = parser.Next(&frame, &close_code);
					if (result == websocket::FrameParser::Result::NeedMore)
					{
						break;
					}

					if (result == websocket::FrameParser::Result::Error)
					{
						SendFrame(c, websocket::EncodeClose(close_code));
						closing = true;
						break;
					}

					if (frame.Opcode == websocket::PING)
					{
						SendFrame(c, websocket::EncodeFrame(websocket::PONG, frame.Payload.data(), frame.Payload.size(), false));
						continue;
					}

					if (frame.Opcode == websocket::PONG)
					{
						continue;
					}

					if (frame.Opcode == websocket::CLOSE)
					{
						// Answer with the same code, then the connection is closed.
						SendFrame(c, websocket::EncodeFrame(websocket::CLOSE, frame.Payload.data(), std::min<size_t>(frame.Payload.size(), 2), false));
						closing = true;
						break;
					}

					if (frame.Opcode == websocket::BINARY)
					{
						SendFrame(c, websocket::EncodeClose(websocket::CLOSE_UNSUPPORTED, "only text messages"));
						closing = true;
						break;
					}

					// text, or the rest of a text
					if ((frame.Opcode == websocket::TEXT) == in_message)
					{
						// A new text in the middle of one, or a continuation without a start.
						SendFrame(c, websocket::EncodeClose(websocket::CLOSE_PROTOCOL_ERROR));
						closing = true;
						break;
					}

					if (message.size() + frame.Payload.size() > m_Config.MaxMessageBytes)
					{
						SendFrame(c, websocket::EncodeClose(websocket::CLOSE_TOO_BIG));
						closing = true;
						break;
					}

					message.append((const char *)frame.Payload.data(), frame.Payload.size());
					in_message = !frame.Fin;
					if (!frame.Fin)
					{
						continue;
					}

					std::string answer;
					bool close_after = false;
					if (m_OnMessage)
					{
						m_OnMessage(c.Session, message, &answer, &close_after);
					}

					message.clear();
					if (!answer.empty() && !SendFrame(c, websocket::EncodeFrame(websocket::TEXT, answer.data(), answer.size(), false)))
					{
						closing = true;
						break;
					}

					if (close_after)
					{
						SendFrame(c, websocket::EncodeClose(websocket::CLOSE_POLICY));
						closing = true;
						break;
					}
				}
			}

			if (!m_Running && !closing)
			{
				// The server is stopping: say so, so the client knows it is not a broken connection.
				SendFrame(c, websocket::EncodeClose(websocket::CLOSE_GOING_AWAY, "server stopping"));
			}

			// Closing a connection with data of the client, which was not read, resets it, and the client may lose the close message with it. So what the
			// client still sends is read (and thrown away) for a moment, until it closes its side, too.
			int64 drain_until = Core::QueryMS() + 300;
			char drain[4096];
			while (Core::QueryMS() < drain_until)
			{
				if (c.Tcp->Receive(drain, sizeof(drain), 50) < 0)
				{
					break;
				}
			}

			if (m_OnClose)
			{
				m_OnClose(c.Session);
			}
		}

		c.Tcp->Close();
		c.Done = true;
	}
}
