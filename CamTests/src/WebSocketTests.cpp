#include "CamTest.h"
#include "TestUtils.h"

#include "Core/CryptoPrimitives.h"
#include "Net/CommandDispatcher.h"
#include "Net/WebSocket.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using Core::Json;
namespace ws = Core::websocket;

// ------------------------------------------------------------------------------------------------------------------------------ the protocol itself

TEST(WebSocket, Base64)
{
	// The examples of RFC 4648.
	auto encode = [](const std::string &text) { return ws::Base64Encode((const Byte *)text.data(), (uint32)text.size()); };
	CHECK_EQ(encode(""), "");
	CHECK_EQ(encode("f"), "Zg==");
	CHECK_EQ(encode("fo"), "Zm8=");
	CHECK_EQ(encode("foo"), "Zm9v");
	CHECK_EQ(encode("foob"), "Zm9vYg==");
	CHECK_EQ(encode("fooba"), "Zm9vYmE=");
	CHECK_EQ(encode("foobar"), "Zm9vYmFy");
}

TEST(WebSocket, TheAcceptKeyOfTheRfcExample)
{
	// RFC 6455, section 1.3.
	CHECK_EQ(ws::AcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocket, ParsesTheHandshakeOfTheRfcExample)
{
	std::string request =
		"GET /chat HTTP/1.1\r\n"
		"Host: server.example.com\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Origin: http://example.com\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";

	ws::HandshakeRequest parsed;
	std::string error;
	REQUIRE(ws::ParseHandshake(request, &parsed, &error));
	CHECK_EQ(parsed.Path, "/chat");
	CHECK_EQ(parsed.Key, "dGhlIHNhbXBsZSBub25jZQ==");
	CHECK_EQ(parsed.Headers["origin"], "http://example.com");

	std::string response = ws::HandshakeResponse(parsed);
	CHECK(response.find("HTTP/1.1 101") == 0);
	CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
	CHECK(response.size() >= 4 && response.substr(response.size() - 4) == "\r\n\r\n");
}

TEST(WebSocket, BrowsersSendAListInTheConnectionHeader)
{
	std::string request =
		"GET / HTTP/1.1\r\nhost: x\r\nupgrade: WebSocket\r\nconnection: keep-alive, Upgrade\r\nsec-websocket-key: abc\r\nsec-websocket-version: 13\r\n\r\n";
	ws::HandshakeRequest parsed;
	std::string error;
	CHECK(ws::ParseHandshake(request, &parsed, &error));
}

TEST(WebSocket, RefusesRequestsWhichAreNoHandshake)
{
	ws::HandshakeRequest parsed;
	std::string error;
	CHECK(!ws::ParseHandshake("GET / HTTP/1.1\r\nHost: x\r\n\r\n", &parsed, &error));											// no upgrade
	CHECK(!ws::ParseHandshake("POST / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: a\r\nSec-WebSocket-Version: 13\r\n\r\n", &parsed, &error));
	CHECK(!ws::ParseHandshake("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n", &parsed, &error));		// no key
	CHECK(!ws::ParseHandshake("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: a\r\nSec-WebSocket-Version: 8\r\n\r\n", &parsed, &error));	// old version
	CHECK(!ws::ParseHandshake("garbage", &parsed, &error));
	CHECK(!ws::ParseHandshake("", &parsed, &error));
	CHECK(!error.empty());
}

TEST(WebSocket, FramesOfTheRfcExamples)
{
	// A masked "Hello" of a client (RFC 6455, section 5.7).
	const Byte masked_hello[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	ws::FrameParser parser;
	parser.Feed(masked_hello, sizeof(masked_hello));
	ws::Frame frame;
	uint16 code = 0;
	REQUIRE(parser.Next(&frame, &code) == ws::FrameParser::Result::Frame);
	CHECK(frame.Fin);
	CHECK_EQ((int)frame.Opcode, (int)ws::TEXT);
	CHECK_EQ(std::string((const char *)frame.Payload.data(), frame.Payload.size()), "Hello");

	// An unmasked "Hello" of a server.
	const Byte hello[] = { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
	std::vector<Byte> encoded = ws::EncodeFrame(ws::TEXT, "Hello", 5, false);
	CHECK(encoded == std::vector<Byte>(hello, hello + sizeof(hello)));

	// A ping.
	const Byte ping[] = { 0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
	CHECK(ws::EncodeFrame(ws::PING, "Hello", 5, false) == std::vector<Byte>(ping, ping + sizeof(ping)));
}

TEST(WebSocket, LengthsOfAllSizes)
{
	for (size_t size : { (size_t)0, (size_t)1, (size_t)125, (size_t)126, (size_t)127, (size_t)65535, (size_t)65536, (size_t)100000 })
	{
		std::vector<Byte> payload(size);
		for (size_t i = 0; i < size; ++i)
		{
			payload[i] = (Byte)(i * 31);
		}

		std::vector<Byte> frame = ws::EncodeFrame(ws::TEXT, payload.data(), payload.size(), true);

		// The header has 1 + 1 (or 3, or 9) bytes, and the mask.
		size_t header = size < 126 ? 2 : (size <= 65535 ? 4 : 10);
		CHECK_EQ(frame.size(), header + 4 + size);

		ws::FrameParser parser(1 << 20, true);
		parser.Feed(frame.data(), frame.size());
		ws::Frame parsed;
		uint16 code = 0;
		REQUIRE(parser.Next(&parsed, &code) == ws::FrameParser::Result::Frame);
		CHECK(parsed.Payload == payload);
	}
}

TEST(WebSocket, FramesCanArriveInPiecesAndTogether)
{
	std::vector<Byte> first = ws::EncodeFrame(ws::TEXT, "first", 5, true);
	std::vector<Byte> second = ws::EncodeFrame(ws::TEXT, "second message", 14, true);
	std::vector<Byte> both = first;
	both.insert(both.end(), second.begin(), second.end());

	// Byte by byte.
	ws::FrameParser parser;
	std::vector<std::string> texts;
	for (Byte b : both)
	{
		parser.Feed(&b, 1);
		ws::Frame frame;
		uint16 code = 0;
		while (parser.Next(&frame, &code) == ws::FrameParser::Result::Frame)
		{
			texts.push_back(std::string((const char *)frame.Payload.data(), frame.Payload.size()));
		}
	}

	REQUIRE_EQ(texts.size(), (size_t)2);
	CHECK_EQ(texts[0], "first");
	CHECK_EQ(texts[1], "second message");
}

TEST(WebSocket, TheParserRefusesBadFrames)
{
	auto result_of = [](const std::vector<Byte> &bytes, uint16 *code, uint64 max = 1 << 20)
	{
		ws::FrameParser parser(max, true);
		parser.Feed(bytes.data(), bytes.size());
		ws::Frame frame;
		return parser.Next(&frame, code);
	};

	uint16 code = 0;

	// Not masked (a client must mask).
	CHECK(result_of(ws::EncodeFrame(ws::TEXT, "x", 1, false), &code) == ws::FrameParser::Result::Error);
	CHECK_EQ((int)code, (int)ws::CLOSE_PROTOCOL_ERROR);

	// Too large (refused from the header on, without waiting for the payload).
	std::vector<Byte> header_only = ws::EncodeFrame(ws::TEXT, std::vector<Byte>(5000).data(), 5000, true);
	header_only.resize(8);
	CHECK(result_of(header_only, &code, 1000) == ws::FrameParser::Result::Error);
	CHECK_EQ((int)code, (int)ws::CLOSE_TOO_BIG);

	// A control frame, which is fragmented or too long.
	CHECK(result_of(ws::EncodeFrame(ws::PING, "x", 1, true, false), &code) == ws::FrameParser::Result::Error);
	std::vector<Byte> long_ping(126);
	CHECK(result_of(ws::EncodeFrame(ws::PING, long_ping.data(), long_ping.size(), true), &code) == ws::FrameParser::Result::Error);

	// An unknown opcode, and reserved bits.
	CHECK(result_of(ws::EncodeFrame(0x3, "x", 1, true), &code) == ws::FrameParser::Result::Error);
	std::vector<Byte> reserved = ws::EncodeFrame(ws::TEXT, "x", 1, true);
	reserved[0] |= 0x40;
	CHECK(result_of(reserved, &code) == ws::FrameParser::Result::Error);

	// A frame, which is not complete yet, is no error.
	std::vector<Byte> partial = ws::EncodeFrame(ws::TEXT, "hello", 5, true);
	partial.pop_back();
	CHECK(result_of(partial, &code) == ws::FrameParser::Result::NeedMore);
}

// ------------------------------------------------------------------------------------------------------------------------------ the commands

namespace
{
	std::string Send(Core::CommandDispatcher &dispatcher, Core::WebSocketSession &session, const std::string &message, bool *close = nullptr)
	{
		bool closed = false;
		std::string answer = dispatcher.Process(&session, message, close ? close : &closed);
		return answer;
	}

	Json ParseAnswer(const std::string &text)
	{
		Json json;
		std::string error;
		REQUIRE(Json::Parse(text, &json, &error));
		return json;
	}
}

TEST(CommandDispatcher, NothingWorksBeforeTheLogin)
{
	Core::CommandDispatcher dispatcher("secret-token-1234");
	dispatcher.Register("status", "x", [](const Json &, const Core::CommandDispatcher::Context &, Json *result, std::string *) { (*result)["value"] = 1; return true; });

	Core::WebSocketSession session;
	Json answer = ParseAnswer(Send(dispatcher, session, "{\"id\":1,\"cmd\":\"status\"}"));
	CHECK(!answer.Get("ok").AsBool(true));
	CHECK_EQ(answer.Get("id").AsInt(), 1);
	CHECK(answer.Get("error").AsString().find("not logged in") != std::string::npos);

	answer = ParseAnswer(Send(dispatcher, session, "{\"cmd\":\"ping\"}"));
	CHECK(!answer.Get("ok").AsBool(true));
	CHECK(!session.Authenticated);
}

TEST(CommandDispatcher, LoginWithTheRightToken)
{
	Core::CommandDispatcher dispatcher("secret-token-1234");
	dispatcher.Register("status", "x", [](const Json &, const Core::CommandDispatcher::Context &, Json *result, std::string *) { (*result)["value"] = 42; return true; });

	Core::WebSocketSession session;
	Json answer = ParseAnswer(Send(dispatcher, session, "{\"id\":\"a\",\"cmd\":\"auth\",\"args\":{\"token\":\"secret-token-1234\"}}"));
	CHECK(answer.Get("ok").AsBool());
	CHECK_EQ(answer.Get("id").AsString(), "a");
	CHECK(session.Authenticated);

	answer = ParseAnswer(Send(dispatcher, session, "{\"id\":2,\"cmd\":\"status\"}"));
	CHECK(answer.Get("ok").AsBool());
	CHECK_EQ(answer.Get("result").Get("value").AsInt(), 42);
	CHECK_EQ(answer.Get("id").AsInt(), 2);
}

TEST(CommandDispatcher, WrongTokensAreRefusedAndThreeEndTheConnection)
{
	Core::CommandDispatcher dispatcher("secret-token-1234");
	Core::WebSocketSession session;
	session.Id = 1;

	for (int attempt = 1; attempt <= 3; ++attempt)
	{
		bool close = false;
		Json answer = ParseAnswer(Send(dispatcher, session, "{\"cmd\":\"auth\",\"args\":{\"token\":\"wrong" + std::to_string(attempt) + "\"}}", &close));
		CHECK(!answer.Get("ok").AsBool(true));
		CHECK_EQ(close, attempt == 3);
	}

	CHECK(!session.Authenticated);

	// Also: no token, a token of the wrong type, and a part of the right one.
	Core::WebSocketSession other;
	other.Id = 2;
	CHECK(!ParseAnswer(Send(dispatcher, other, "{\"cmd\":\"auth\"}")).Get("ok").AsBool(true));
	CHECK(!ParseAnswer(Send(dispatcher, other, "{\"cmd\":\"auth\",\"args\":{\"token\":12}}")).Get("ok").AsBool(true));
	CHECK(!ParseAnswer(Send(dispatcher, other, "{\"cmd\":\"auth\",\"args\":{\"token\":\"secret-token-123\"}}")).Get("ok").AsBool(true));
	CHECK(!other.Authenticated);
}

TEST(CommandDispatcher, WithoutATokenNobodyGetsIn)
{
	Core::CommandDispatcher dispatcher("");
	Core::WebSocketSession session;
	bool close = false;
	Json answer = ParseAnswer(Send(dispatcher, session, "{\"cmd\":\"auth\",\"args\":{\"token\":\"\"}}", &close));
	CHECK(!answer.Get("ok").AsBool(true));
	CHECK(close);
	CHECK(!session.Authenticated);
}

TEST(CommandDispatcher, BrokenMessagesGetAnError)
{
	Core::CommandDispatcher dispatcher("secret-token-1234");
	Core::WebSocketSession session;
	session.Authenticated = true;

	for (const char *message : { "not json", "[1,2]", "42", "{}", "{\"cmd\":5}", "{\"cmd\":\"\"}" })
	{
		Json answer = ParseAnswer(Send(dispatcher, session, message));
		CHECK(!answer.Get("ok").AsBool(true));
		CHECK(!answer.Get("error").AsString().empty());
	}
}

TEST(CommandDispatcher, UnknownCommandAndHelp)
{
	Core::CommandDispatcher dispatcher("secret-token-1234");
	dispatcher.Register("save", "saves videos", [](const Json &, const Core::CommandDispatcher::Context &, Json *, std::string *) { return true; });
	Core::WebSocketSession session;
	session.Authenticated = true;

	Json answer = ParseAnswer(Send(dispatcher, session, "{\"cmd\":\"explode\"}"));
	CHECK(!answer.Get("ok").AsBool(true));
	CHECK(answer.Get("error").AsString().find("explode") != std::string::npos);

	answer = ParseAnswer(Send(dispatcher, session, "{\"cmd\":\"help\"}"));
	REQUIRE(answer.Get("ok").AsBool());
	const Json &commands = answer.Get("result").Get("commands");
	bool has_save = false, has_ping = false;
	for (const Json &command : commands.Items())
	{
		has_save = has_save || (command.Get("cmd").AsString() == "save" && command.Get("description").AsString() == "saves videos");
		has_ping = has_ping || command.Get("cmd").AsString() == "ping";
	}

	CHECK(has_save);
	CHECK(has_ping);
}

TEST(CommandDispatcher, ArgumentsReachTheHandlerAndErrorsComeBack)
{
	Core::CommandDispatcher dispatcher("secret-token-1234");
	dispatcher.Register("add", "adds", [](const Json &args, const Core::CommandDispatcher::Context &context, Json *result, std::string *error)
	{
		if (!args.Get("a").IsNumber())
		{
			*error = "a is missing";
			return false;
		}

		(*result)["sum"] = args.Get("a").AsInt() + args.Get("b").AsInt();
		(*result)["from_loopback"] = context.IsLoopback;
		return true;
	});

	Core::WebSocketSession session;
	session.Authenticated = true;
	session.IsLoopback = true;

	Json answer = ParseAnswer(Send(dispatcher, session, "{\"cmd\":\"add\",\"args\":{\"a\":2,\"b\":3}}"));
	CHECK_EQ(answer.Get("result").Get("sum").AsInt(), 5);
	CHECK(answer.Get("result").Get("from_loopback").AsBool());
	CHECK(!answer.Has("id"));		// no id, no id in the answer

	answer = ParseAnswer(Send(dispatcher, session, "{\"id\":9,\"cmd\":\"add\"}"));
	CHECK(!answer.Get("ok").AsBool(true));
	CHECK_EQ(answer.Get("error").AsString(), "a is missing");
	CHECK_EQ(answer.Get("id").AsInt(), 9);
}

TEST(CommandDispatcher, AnExceptionInACommandIsAnErrorNotACrash)
{
	Core::CommandDispatcher dispatcher("secret-token-1234");
	dispatcher.Register("boom", "throws", [](const Json &, const Core::CommandDispatcher::Context &, Json *, std::string *) -> bool { throw std::runtime_error("something broke"); });
	Core::WebSocketSession session;
	session.Authenticated = true;

	Json answer = ParseAnswer(Send(dispatcher, session, "{\"cmd\":\"boom\"}"));
	CHECK(!answer.Get("ok").AsBool(true));
	CHECK(answer.Get("error").AsString().find("something broke") != std::string::npos);
}

TEST(CommandDispatcher, EventsHaveAFixedShape)
{
	Json data = Json::Object();
	data["camera"] = "Front door";
	CHECK_EQ(Core::CommandDispatcher::MakeEvent("camera_connected", data), "{\"event\":\"camera_connected\",\"data\":{\"camera\":\"Front door\"}}");
}

// ------------------------------------------------------------------------------------------------------------------------------ the server

namespace
{
	// A client of the WebSocket, as a browser or a script would be.
	class Client
	{
	public:

		explicit Client(uint16 port)
		{
			m_Tcp = Core::TcpConnection::Connect("127.0.0.1", port, 2000);
		}

		bool Connected() const { return m_Tcp != nullptr; }

		// The handshake. Returns the first line of the answer ("HTTP/1.1 101 Switching Protocols").
		std::string Handshake(const std::string &extraHeaders = "")
		{
			Byte random[16];
			Core::crypto::RandomBytes(random, 16);
			std::string key = ws::Base64Encode(random, 16);

			std::string request = "GET /control HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n" + extraHeaders + "\r\n";
			return Exchange(request, key);
		}

		// Sends any bytes as the request, and reads the answer up to the end of its header.
		std::string Exchange(const std::string &request, const std::string &key = "")
		{
			m_Tcp->Send(request.data(), (uint32)request.size());

			std::string answer;
			char buffer[1024];
			auto start = std::chrono::steady_clock::now();
			while (answer.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
			{
				int32 received = m_Tcp->Receive(buffer, sizeof(buffer), 100);
				if (received < 0)
				{
					break;
				}

				answer.append(buffer, (size_t)received);
			}

			std::string header = answer.substr(0, answer.find("\r\n\r\n") == std::string::npos ? answer.size() : answer.find("\r\n\r\n") + 4);
			m_Leftover = answer.size() > header.size() ? answer.substr(header.size()) : "";
			if (!key.empty() && header.find("Sec-WebSocket-Accept: " + ws::AcceptKey(key)) == std::string::npos)
			{
				return "WRONG ACCEPT: " + header;
			}

			if (!m_Leftover.empty())
			{
				m_Parser.Feed(m_Leftover.data(), m_Leftover.size());
			}

			return header.substr(0, header.find("\r\n"));
		}

		void SendFrame(uint8 opcode, const std::string &payload, bool fin = true, bool mask = true)
		{
			std::vector<Byte> frame = ws::EncodeFrame(opcode, payload.data(), payload.size(), mask, fin);
			m_Tcp->Send(frame.data(), (uint32)frame.size());
		}

		void SendText(const std::string &text) { SendFrame(ws::TEXT, text); }

		// The next frame of the server (without a mask). Returns false if nothing came in time, or the connection is closed.
		bool ReceiveFrame(ws::Frame *frame, int timeoutMS = 3000, bool *connectionClosed = nullptr)
		{
			auto start = std::chrono::steady_clock::now();
			for (;;)
			{
				uint16 code = 0;
				if (m_Parser.Next(frame, &code) == ws::FrameParser::Result::Frame)
				{
					return true;
				}

				if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMS))
				{
					return false;
				}

				char buffer[4096];
				int32 received = m_Tcp->Receive(buffer, sizeof(buffer), 50);
				if (received < 0)
				{
					if (connectionClosed)
					{
						*connectionClosed = true;
					}

					return false;
				}

				if (received > 0)
				{
					m_Parser.Feed(buffer, (size_t)received);
				}
			}
		}

		// A text message of the server (other frames are skipped).
		bool ReceiveText(std::string *text, int timeoutMS = 3000)
		{
			ws::Frame frame;
			while (ReceiveFrame(&frame, timeoutMS))
			{
				if (frame.Opcode == ws::TEXT)
				{
					text->assign((const char *)frame.Payload.data(), frame.Payload.size());
					return true;
				}
			}

			return false;
		}

		Json Ask(const std::string &request)
		{
			SendText(request);
			std::string text;
			REQUIRE(ReceiveText(&text));
			Json json;
			std::string error;
			REQUIRE(Json::Parse(text, &json, &error));
			return json;
		}

		bool Login(const std::string &token)
		{
			return Ask("{\"cmd\":\"auth\",\"args\":{\"token\":\"" + token + "\"}}").Get("ok").AsBool();
		}

		// True, when the server has closed the connection.
		bool WaitUntilClosed(int timeoutMS = 3000)
		{
			auto start = std::chrono::steady_clock::now();
			while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMS))
			{
				char buffer[1024];
				int32 received = m_Tcp->Receive(buffer, sizeof(buffer), 50);
				if (received < 0)
				{
					return true;
				}
			}

			return false;
		}

	private:

		std::unique_ptr<Core::TcpConnection> m_Tcp;
		std::string m_Leftover;
		ws::FrameParser m_Parser{ 1 << 20, false };
	};

	// A server with a few commands.
	struct Rig
	{
		static constexpr const char *TOKEN = "0123456789abcdef0123456789abcdef";

		Core::CommandDispatcher Dispatcher{ TOKEN };
		std::unique_ptr<Core::WebSocketServer> Server;
		std::atomic<int> Opened{ 0 }, Closed{ 0 };

		explicit Rig(Core::WebSocketServerConfig config = Core::WebSocketServerConfig())
		{
			config.Port = 0;
			Dispatcher.Register("echo", "answers with its arguments", [](const Json &args, const Core::CommandDispatcher::Context &, Json *result, std::string *)
			{
				(*result)["args"] = args;
				return true;
			});
			Dispatcher.Register("slow", "takes a moment", [](const Json &, const Core::CommandDispatcher::Context &, Json *result, std::string *)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				(*result)["done"] = true;
				return true;
			});

			Server = std::make_unique<Core::WebSocketServer>(config, Dispatcher.AsMessageHandler());
			Server->SetSessionHandlers([this](Core::WebSocketSession &) { ++Opened; }, [this](Core::WebSocketSession &) { ++Closed; });
			REQUIRE(Server->Start());
		}

		uint16 Port() const { return Server->Port(); }
	};

	bool WaitFor(const std::function<bool()> &condition, int timeoutMS = 3000)
	{
		auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMS))
		{
			if (condition())
			{
				return true;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		return condition();
	}
}

TEST(WebSocketServer, ConnectLoginAndGiveCommands)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE(client.Connected());
	CHECK_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");

	CHECK(client.Login(Rig::TOKEN));

	Json answer = client.Ask("{\"id\":5,\"cmd\":\"echo\",\"args\":{\"text\":\"h\\u00e4llo\",\"list\":[1,2]}}");
	CHECK(answer.Get("ok").AsBool());
	CHECK_EQ(answer.Get("id").AsInt(), 5);
	CHECK_EQ(answer.Get("result").Get("args").Get("text").AsString(), "h\xC3\xA4llo");
	CHECK_EQ(answer.Get("result").Get("args").Get("list").Size(), (size_t)2);

	CHECK(client.Ask("{\"cmd\":\"ping\"}").Get("result").Get("pong").AsBool());
	CHECK(WaitFor([&] { return rig.Opened == 1; }));
}

TEST(WebSocketServer, WrongTokenAndCommandsBeforeLogin)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE(client.Connected());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");

	CHECK(!client.Ask("{\"cmd\":\"echo\"}").Get("ok").AsBool(true));
	CHECK(!client.Login("wrong"));
	CHECK(client.Login(Rig::TOKEN));		// a typo is not the end, if it is not 3 times
	CHECK(client.Ask("{\"cmd\":\"echo\"}").Get("ok").AsBool());
}

TEST(WebSocketServer, ThreeWrongTokensEndTheConnection)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");

	for (int i = 0; i < 3; ++i)
	{
		client.Login("wrong");
	}

	CHECK(client.WaitUntilClosed());
	CHECK(WaitFor([&] { return rig.Closed == 1; }));
}

TEST(WebSocketServer, PlainHttpGetsAnAnswerNotASocket)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE(client.Connected());
	std::string first = client.Exchange("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
	CHECK(first.find("400") != std::string::npos);
	CHECK(client.WaitUntilClosed());
}

TEST(WebSocketServer, GarbageInsteadOfAHandshakeIsDropped)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE(client.Connected());
	client.Exchange(std::string(100, 'x') + "\r\n\r\n");
	CHECK(client.WaitUntilClosed());

	// The server still works.
	Client second(rig.Port());
	CHECK_EQ(second.Handshake(), "HTTP/1.1 101 Switching Protocols");
}

TEST(WebSocketServer, PingIsAnsweredWithPong)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");

	client.SendFrame(ws::PING, "are you there");
	ws::Frame frame;
	REQUIRE(client.ReceiveFrame(&frame));
	CHECK_EQ((int)frame.Opcode, (int)ws::PONG);
	CHECK_EQ(std::string((const char *)frame.Payload.data(), frame.Payload.size()), "are you there");
}

TEST(WebSocketServer, MessagesInSeveralFramesAreJoined)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");
	REQUIRE(client.Login(Rig::TOKEN));

	std::string message = "{\"id\":1,\"cmd\":\"echo\",\"args\":{\"text\":\"in parts\"}}";
	client.SendFrame(ws::TEXT, message.substr(0, 10), false);
	client.SendFrame(ws::PING, "in between");		// a ping may come between the parts
	client.SendFrame(ws::CONTINUATION, message.substr(10, 15), false);
	client.SendFrame(ws::CONTINUATION, message.substr(25), true);

	std::string text;
	REQUIRE(client.ReceiveText(&text));
	Json answer;
	std::string error;
	REQUIRE(Json::Parse(text, &answer, &error));
	CHECK_EQ(answer.Get("result").Get("args").Get("text").AsString(), "in parts");
}

TEST(WebSocketServer, ProtocolViolationsCloseTheConnection)
{
	{
		// A message which is not masked.
		Rig rig;
		Client client(rig.Port());
		REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");
		client.SendFrame(ws::TEXT, "{}", true, false);
		ws::Frame frame;
		REQUIRE(client.ReceiveFrame(&frame));
		CHECK_EQ((int)frame.Opcode, (int)ws::CLOSE);
		CHECK_EQ((int)(((int)frame.Payload[0] << 8) | frame.Payload[1]), (int)ws::CLOSE_PROTOCOL_ERROR);
		CHECK(client.WaitUntilClosed());
	}

	{
		// A binary message (only text is understood).
		Rig rig;
		Client client(rig.Port());
		REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");
		client.SendFrame(ws::BINARY, "data");
		ws::Frame frame;
		REQUIRE(client.ReceiveFrame(&frame));
		CHECK_EQ((int)frame.Opcode, (int)ws::CLOSE);
		CHECK_EQ((int)(((int)frame.Payload[0] << 8) | frame.Payload[1]), (int)ws::CLOSE_UNSUPPORTED);
	}

	{
		// A continuation without a start.
		Rig rig;
		Client client(rig.Port());
		REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");
		client.SendFrame(ws::CONTINUATION, "orphan");
		ws::Frame frame;
		REQUIRE(client.ReceiveFrame(&frame));
		CHECK_EQ((int)frame.Opcode, (int)ws::CLOSE);
	}
}

TEST(WebSocketServer, TooLargeMessagesAreRefused)
{
	Core::WebSocketServerConfig config;
	config.MaxMessageBytes = 1000;
	Rig rig(config);

	Client client(rig.Port());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");
	client.SendText(std::string(5000, 'x'));

	ws::Frame frame;
	REQUIRE(client.ReceiveFrame(&frame));
	CHECK_EQ((int)frame.Opcode, (int)ws::CLOSE);
	CHECK_EQ((int)(((int)frame.Payload[0] << 8) | frame.Payload[1]), (int)ws::CLOSE_TOO_BIG);
}

TEST(WebSocketServer, TheClientCanCloseProperly)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");
	REQUIRE(client.Login(Rig::TOKEN));

	std::string reason(2, 0);
	reason[0] = (char)(ws::CLOSE_NORMAL >> 8);
	reason[1] = (char)(ws::CLOSE_NORMAL & 0xFF);
	client.SendFrame(ws::CLOSE, reason);

	ws::Frame frame;
	REQUIRE(client.ReceiveFrame(&frame));
	CHECK_EQ((int)frame.Opcode, (int)ws::CLOSE);
	CHECK(client.WaitUntilClosed());
	CHECK(WaitFor([&] { return rig.Closed == 1; }));
}

TEST(WebSocketServer, ClientsWhichDoNotLogInAreDisconnected)
{
	Core::WebSocketServerConfig config;
	config.AuthenticationTimeoutMS = 400;
	Rig rig(config);

	Client client(rig.Port());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");

	ws::Frame frame;
	REQUIRE(client.ReceiveFrame(&frame, 3000));
	CHECK_EQ((int)frame.Opcode, (int)ws::CLOSE);
	CHECK_EQ((int)(((int)frame.Payload[0] << 8) | frame.Payload[1]), (int)ws::CLOSE_POLICY);
}

TEST(WebSocketServer, ClientsWhichNeverFinishTheHandshakeAreDropped)
{
	Core::WebSocketServerConfig config;
	config.HandshakeTimeoutMS = 300;
	Rig rig(config);

	Client client(rig.Port());
	REQUIRE(client.Connected());
	// Nothing is sent at all.
	CHECK(client.WaitUntilClosed(3000));
}

TEST(WebSocketServer, TooManyConnectionsAreRefused)
{
	Core::WebSocketServerConfig config;
	config.MaxConnections = 2;
	Rig rig(config);

	Client first(rig.Port()), second(rig.Port());
	REQUIRE_EQ(first.Handshake(), "HTTP/1.1 101 Switching Protocols");
	REQUIRE_EQ(second.Handshake(), "HTTP/1.1 101 Switching Protocols");
	REQUIRE(WaitFor([&] { return rig.Server->ConnectionCount() == 2; }));

	Client third(rig.Port());
	REQUIRE(third.Connected());
	std::string answer = third.Handshake();
	CHECK(answer.find("503") != std::string::npos);

	// When one is gone, there is room again.
	first.SendFrame(ws::CLOSE, std::string(2, '\0'));
	REQUIRE(WaitFor([&] { return rig.Server->ConnectionCount() == 1; }));
	Client fourth(rig.Port());
	CHECK_EQ(fourth.Handshake(), "HTTP/1.1 101 Switching Protocols");
}

TEST(WebSocketServer, SeveralClientsAtTheSameTime)
{
	Rig rig;
	std::vector<std::unique_ptr<Client>> clients;
	for (int i = 0; i < 4; ++i)
	{
		clients.push_back(std::make_unique<Client>(rig.Port()));
		REQUIRE_EQ(clients.back()->Handshake(), "HTTP/1.1 101 Switching Protocols");
		REQUIRE(clients.back()->Login(Rig::TOKEN));
	}

	// All ask at once, one command takes a moment: they do not wait for each other.
	auto start = std::chrono::steady_clock::now();
	for (auto &client : clients)
	{
		client->SendText("{\"cmd\":\"slow\"}");
	}

	for (auto &client : clients)
	{
		std::string text;
		REQUIRE(client->ReceiveText(&text));
		CHECK(text.find("\"done\":true") != std::string::npos);
	}

	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	CHECK(elapsed < 1100);		// 4 times 300 ms one after the other would be 1200 ms
}

TEST(WebSocketServer, BroadcastReachesOnlyLoggedInClients)
{
	Rig rig;
	Client in(rig.Port()), out(rig.Port());
	REQUIRE_EQ(in.Handshake(), "HTTP/1.1 101 Switching Protocols");
	REQUIRE_EQ(out.Handshake(), "HTTP/1.1 101 Switching Protocols");
	REQUIRE(in.Login(Rig::TOKEN));

	Json data = Json::Object();
	data["camera"] = "Front door";
	rig.Server->Broadcast(Core::CommandDispatcher::MakeEvent("camera_connected", data));

	std::string text;
	REQUIRE(in.ReceiveText(&text));
	CHECK_EQ(text, "{\"event\":\"camera_connected\",\"data\":{\"camera\":\"Front door\"}}");

	std::string nothing;
	CHECK(!out.ReceiveText(&nothing, 300));
}

TEST(WebSocketServer, StopDisconnectsEveryone)
{
	Rig rig;
	Client client(rig.Port());
	REQUIRE_EQ(client.Handshake(), "HTTP/1.1 101 Switching Protocols");
	REQUIRE(client.Login(Rig::TOKEN));

	rig.Server->Stop();
	CHECK(client.WaitUntilClosed());
	CHECK_EQ(rig.Server->ConnectionCount(), 0u);

	// And nobody can connect anymore.
	Client late(rig.Port());
	CHECK(!late.Connected());
}
