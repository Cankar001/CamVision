#include "CamTest.h"
#include "TestUtils.h"

#include "Net/CamMessages.h"
#include "Net/DeviceRegistry.h"
#include "Net/SecureSocket.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

// The secure connection between the devices (cameras and displays) and the server, over real sockets on the loopback address. The attacks of somebody in the
// network are played with a "tap" between them, which sees every datagram, and can change, drop and repeat them.

namespace
{
	uint16 NextPort()
	{
		static uint16 port = 46100;
		return port++;
	}

	int64 NowMS()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::vector<Byte> Message(const std::string &text)
	{
		return std::vector<Byte>(text.begin(), text.end());
	}

	// A message of the programs: the header of the protocol and some text.
	std::vector<Byte> AppMessage(MessageType type, const std::string &text)
	{
		header_t header = {};
		header.Version = CAM_PROTOCOL_VERSION;
		header.Type = type;

		std::vector<Byte> bytes(sizeof(header) + text.size());
		memcpy(bytes.data(), &header, sizeof(header));
		memcpy(bytes.data() + sizeof(header), text.data(), text.size());
		return bytes;
	}

	std::string TextOf(const std::vector<Byte> &message)
	{
		return std::string((const char *)message.data() + sizeof(header_t), message.size() - sizeof(header_t));
	}

	// A network between a device and the server: everything goes through it.
	class Tap
	{
	public:

		Tap(uint16 frontPort, uint16 serverPort)
		{
			m_Front.reset(Core::Socket::Create());
			m_Front->Open();
			m_Front->BindLoopback(frontPort);
			m_Front->SetNonBlocking(true);

			m_Back.reset(Core::Socket::Create());
			m_Back->Open(true, "127.0.0.1", serverPort);
			m_Back->SetNonBlocking(true);
			m_Server = m_Back->Lookup("127.0.0.1", serverPort);
		}

		// Moves all waiting datagrams on.
		void Pump()
		{
			std::vector<Byte> buffer(65536);
			for (;;)
			{
				Core::addr_t from = {};
				int32 length = m_Front->Recv(buffer.data(), (int32)buffer.size(), &from);
				if (length <= 0)
				{
					break;
				}

				m_Client = from;
				std::vector<Byte> packet(buffer.begin(), buffer.begin() + length);
				ToServer.push_back(packet);
				if (!OnToServer || OnToServer(packet))
				{
					m_Back->Send(packet.data(), (int32)packet.size(), m_Server);
				}
			}

			for (;;)
			{
				Core::addr_t from = {};
				int32 length = m_Back->Recv(buffer.data(), (int32)buffer.size(), &from);
				if (length <= 0)
				{
					break;
				}

				std::vector<Byte> packet(buffer.begin(), buffer.begin() + length);
				ToClient.push_back(packet);
				if (!OnToClient || OnToClient(packet))
				{
					m_Front->Send(packet.data(), (int32)packet.size(), m_Client);
				}
			}
		}

		// Sends a datagram again, as the device (same address as before).
		void InjectToServer(const std::vector<Byte> &packet)
		{
			m_Back->Send(packet.data(), (int32)packet.size(), m_Server);
		}

		std::vector<std::vector<Byte>> ToServer;
		std::vector<std::vector<Byte>> ToClient;

		// Return false to drop the datagram. The datagram can be changed.
		std::function<bool(std::vector<Byte> &)> OnToServer;
		std::function<bool(std::vector<Byte> &)> OnToClient;

	private:

		std::unique_ptr<Core::Socket> m_Front, m_Back;
		Core::addr_t m_Server = {}, m_Client = {};
	};

	// A server and its devices.
	struct Rig
	{
		TempDir Dir;
		Core::DeviceRegistry Registry;
		Core::Device Camera, Display;
		uint16 ServerPort = NextPort();
		std::unique_ptr<Core::SecureSocket> Server;

		Rig()
			: Registry(Dir.File("devices.cfg"))
		{
			std::string error;
			REQUIRE(Registry.Add(Core::DeviceRole::Camera, "Front door", &Camera, &error));
			REQUIRE(Registry.Add(Core::DeviceRole::Display, "Living room", &Display, &error));
			StartServer();
		}

		void StartServer()
		{
			Server.reset();

			Core::Socket *raw = Core::Socket::Create();
			REQUIRE(raw->Open());
			REQUIRE(raw->BindLoopback(ServerPort));
			REQUIRE(raw->SetNonBlocking(true));
			Server.reset(Core::SecureSocket::CreateServer(raw, &Registry));
		}

		// A device, which talks to the server (or to a tap, which is placed in front of it).
		std::unique_ptr<Core::SecureSocket> MakeDevice(const Core::Device &device, Core::addr_t *serverAddress, uint16 port = 0)
		{
			Core::Socket *raw = Core::Socket::Create();
			REQUIRE(raw->Open(true, "127.0.0.1", port ? port : ServerPort));
			REQUIRE(raw->SetNonBlocking(true));
			*serverAddress = raw->Lookup("127.0.0.1", port ? port : ServerPort);
			return std::unique_ptr<Core::SecureSocket>(Core::SecureSocket::CreateClient(raw, device.Key));
		}
	};

	// The device sends its message again and again (like the programs do with their requests), until the server has it. Returns false after the timeout.
	bool Deliver(Rig &rig, Core::SecureSocket *device, Core::addr_t serverAddress, const std::vector<Byte> &message, std::vector<Byte> *received, Core::addr_t *from, Tap *tap = nullptr, int timeoutMS = 4000)
	{
		int64 start = NowMS();
		std::vector<Byte> buffer(65536);
		while (NowMS() - start < timeoutMS)
		{
			device->Send(message.data(), (int32)message.size(), serverAddress);
			if (tap)
			{
				tap->Pump();
			}

			Core::addr_t addr = {};
			int32 length = rig.Server->Recv(buffer.data(), (int32)buffer.size(), &addr);
			if (length > 0)
			{
				received->assign(buffer.begin(), buffer.begin() + length);
				*from = addr;
				return true;
			}

			// The device needs to read the answers of the handshake.
			Core::addr_t ignored = {};
			device->Recv(buffer.data(), (int32)buffer.size(), &ignored);
			if (tap)
			{
				tap->Pump();
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		return false;
	}

	// Waits for a message at the device.
	bool Receive(Core::SecureSocket *device, std::vector<Byte> *received, Tap *tap = nullptr, int timeoutMS = 2000)
	{
		int64 start = NowMS();
		std::vector<Byte> buffer(65536);
		while (NowMS() - start < timeoutMS)
		{
			if (tap)
			{
				tap->Pump();
			}

			Core::addr_t from = {};
			int32 length = device->Recv(buffer.data(), (int32)buffer.size(), &from);
			if (length > 0)
			{
				received->assign(buffer.begin(), buffer.begin() + length);
				return true;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		return false;
	}

	// Nothing arrives within a short time.
	bool NothingArrives(Core::SecureSocket *socket, Tap *tap = nullptr, int timeoutMS = 300)
	{
		std::vector<Byte> received;
		return !Receive(socket, &received, tap, timeoutMS);
	}
}

TEST(SecureSocket, ADeviceConnectsAndTalksBothWays)
{
	Rig rig;
	Core::addr_t serverAddress = {};
	auto camera = rig.MakeDevice(rig.Camera, &serverAddress);

	std::vector<Byte> received;
	Core::addr_t from = {};
	std::vector<Byte> hello = AppMessage(MessageType::CLIENT_CONNECTION_START, "Front door");
	REQUIRE(Deliver(rig, camera.get(), serverAddress, hello, &received, &from));
	CHECK(received == hello);

	// The server knows, who is behind the address: the name and the role of the device, from its own list.
	Core::SecureSocket::Peer peer;
	REQUIRE(rig.Server->GetPeer(from, &peer));
	CHECK_EQ(peer.Name, "Front door");
	CHECK(peer.Role == Core::DeviceRole::Camera);
	CHECK_EQ(rig.Server->PeerCount(), 1u);

	// And the answer of the server arrives, so the device knows, that the server has its key.
	std::vector<Byte> answer = AppMessage(MessageType::SERVER_CONNECTION_START, "accepted");
	CHECK_EQ(rig.Server->Send(answer.data(), (int32)answer.size(), from), (int32)answer.size());

	std::vector<Byte> got;
	REQUIRE(Receive(camera.get(), &got));
	CHECK(got == answer);
	CHECK(camera->IsEstablished());
}

TEST(SecureSocket, ADisplayHasTheRoleOfADisplay)
{
	Rig rig;
	Core::addr_t serverAddress = {};
	auto display = rig.MakeDevice(rig.Display, &serverAddress);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, display.get(), serverAddress, AppMessage(MessageType::DISPLAY_CONNECTION_START, "x"), &received, &from));

	Core::SecureSocket::Peer peer;
	REQUIRE(rig.Server->GetPeer(from, &peer));
	CHECK_EQ(peer.Name, "Living room");
	CHECK(peer.Role == Core::DeviceRole::Display);
}

TEST(SecureSocket, LargeMessagesTravel)
{
	Rig rig;
	Core::addr_t serverAddress = {};
	auto camera = rig.MakeDevice(rig.Camera, &serverAddress);

	// A chunk of a frame: the header and 1200 bytes of picture.
	std::vector<Byte> chunk = AppMessage(MessageType::CLIENT_FRAME, std::string(1252, 'x'));
	for (size_t i = 0; i < chunk.size(); ++i)
	{
		chunk[i] = (Byte)(chunk[i] ^ (i * 7));
	}

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, camera.get(), serverAddress, chunk, &received, &from));
	CHECK(received == chunk);
}

TEST(SecureSocket, NothingOnTheWireIsReadable)
{
	Rig rig;
	Core::addr_t serverAddress = {};

	// The device talks to the front port of the spy, the spy forwards to the server.
	uint16 front = NextPort();
	Tap spy(front, rig.ServerPort);
	auto device = rig.MakeDevice(rig.Camera, &serverAddress, front);

	const std::string secret = "SECRET-PICTURE-OF-THE-LIVING-ROOM";
	std::vector<Byte> message = AppMessage(MessageType::CLIENT_FRAME, secret);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, device.get(), serverAddress, message, &received, &from, &spy));
	CHECK(received == message);

	// Everything, which the spy saw (both directions), is without the text of the message.
	REQUIRE(!spy.ToServer.empty());
	for (const auto &packet : spy.ToServer)
	{
		std::string raw(packet.begin(), packet.end());
		CHECK(raw.find(secret) == std::string::npos);
	}
}

TEST(SecureSocket, ADeviceKeyIsNeverSent)
{
	Rig rig;
	uint16 front = NextPort();
	Tap spy(front, rig.ServerPort);
	Core::addr_t serverAddress = {};
	auto device = rig.MakeDevice(rig.Camera, &serverAddress, front);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "x"), &received, &from, &spy));

	std::string key((const char *)rig.Camera.Key, 32);
	for (const auto &packet : spy.ToServer)
	{
		CHECK(std::string(packet.begin(), packet.end()).find(key) == std::string::npos);
	}

	for (const auto &packet : spy.ToClient)
	{
		CHECK(std::string(packet.begin(), packet.end()).find(key) == std::string::npos);
	}
}

TEST(SecureSocket, ADeviceWithAnUnknownKeyGetsNothing)
{
	Rig rig;
	Core::Device stranger;
	stranger.Role = Core::DeviceRole::Camera;
	REQUIRE(Core::crypto::RandomBytes(stranger.Key, 32));

	Core::addr_t serverAddress = {};
	auto device = rig.MakeDevice(stranger, &serverAddress);

	std::vector<Byte> received;
	Core::addr_t from = {};
	CHECK(!Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_CONNECTION_START, "intruder"), &received, &from, nullptr, 800));
	CHECK_EQ(rig.Server->PeerCount(), 0u);
	CHECK(rig.Server->RejectedCount() > 0);
	CHECK(!device->IsEstablished());
}

TEST(SecureSocket, MessagesWithoutAKeyAreNotDelivered)
{
	Rig rig;

	// A program without a key (or somebody, who sends the old, plain messages) is not heard.
	std::unique_ptr<Core::Socket> plain(Core::Socket::Create());
	REQUIRE(plain->Open(true, "127.0.0.1", rig.ServerPort));
	Core::addr_t server = plain->Lookup("127.0.0.1", rig.ServerPort);

	std::vector<Byte> message = AppMessage(MessageType::CLIENT_CONNECTION_START, "plain camera");
	plain->Send(message.data(), (int32)message.size(), server);
	std::vector<Byte> heartbeat = AppMessage(MessageType::CLIENT_HEARTBEAT, "");
	plain->Send(heartbeat.data(), (int32)heartbeat.size(), server);

	CHECK(NothingArrives(rig.Server.get()));
	CHECK(rig.Server->RejectedCount() >= 2);
}

TEST(SecureSocket, ADamagedMessageIsDropped)
{
	Rig rig;
	uint16 front = NextPort();
	Tap tap(front, rig.ServerPort);
	Core::addr_t serverAddress = {};
	auto device = rig.MakeDevice(rig.Camera, &serverAddress, front);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "first"), &received, &from, &tap));

	// From now on, the network changes one byte of every message of the device.
	tap.OnToServer = [](std::vector<Byte> &packet)
	{
		if (packet.size() > 20)
		{
			packet[packet.size() / 2] ^= 0x01;
		}

		return true;
	};

	uint64 rejected_before = rig.Server->RejectedCount();
	std::vector<Byte> damaged = AppMessage(MessageType::CLIENT_FRAME, "this arrives damaged");
	for (int i = 0; i < 5; ++i)
	{
		device->Send(damaged.data(), (int32)damaged.size(), serverAddress);
		tap.Pump();
	}

	CHECK(NothingArrives(rig.Server.get(), &tap));
	CHECK(rig.Server->RejectedCount() > rejected_before);

	// When the network is fine again, the connection goes on (nothing was broken by the attempts).
	tap.OnToServer = nullptr;
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "second"), &received, &from, &tap));
	CHECK_EQ(TextOf(received), "second");
}

TEST(SecureSocket, ARepeatedMessageIsDeliveredOnce)
{
	Rig rig;
	uint16 front = NextPort();
	Tap tap(front, rig.ServerPort);
	Core::addr_t serverAddress = {};
	auto device = rig.MakeDevice(rig.Camera, &serverAddress, front);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "warm up"), &received, &from, &tap));

	// A message, which an attacker recorded.
	tap.ToServer.clear();
	std::vector<Byte> frame = AppMessage(MessageType::CLIENT_FRAME, "open the door");
	device->Send(frame.data(), (int32)frame.size(), serverAddress);
	tap.Pump();

	std::vector<Byte> first;
	REQUIRE(Receive(rig.Server.get(), &first, &tap));
	CHECK(first == frame);
	REQUIRE(!tap.ToServer.empty());
	std::vector<Byte> recorded = tap.ToServer.back();

	// Played again (more than once): the server does not accept it.
	uint64 rejected_before = rig.Server->RejectedCount();
	for (int i = 0; i < 3; ++i)
	{
		tap.InjectToServer(recorded);
	}

	CHECK(NothingArrives(rig.Server.get(), &tap));
	CHECK(rig.Server->RejectedCount() >= rejected_before + 3);
}

TEST(SecureSocket, MessagesOfAnOldConnectionAreUseless)
{
	Rig rig;
	uint16 front = NextPort();
	Tap tap(front, rig.ServerPort);
	Core::addr_t serverAddress = {};

	// A first connection, which an attacker records.
	auto first = rig.MakeDevice(rig.Camera, &serverAddress, front);
	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, first.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "one"), &received, &from, &tap));
	std::vector<std::vector<Byte>> recording = tap.ToServer;
	REQUIRE(!recording.empty());

	// Later the same device (same key) connects again: the keys of the connection are new, because both sides contribute new random numbers.
	first.reset();
	auto second = rig.MakeDevice(rig.Camera, &serverAddress, front);
	REQUIRE(Deliver(rig, second.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "two"), &received, &from, &tap));
	CHECK(NothingArrives(rig.Server.get(), &tap, 100));

	// The recording of the first connection (hello and data) is played: nothing is delivered.
	uint64 rejected_before = rig.Server->RejectedCount();
	for (const auto &packet : recording)
	{
		tap.InjectToServer(packet);
	}

	CHECK(NothingArrives(rig.Server.get(), &tap));
	CHECK(rig.Server->RejectedCount() > rejected_before);
}

TEST(SecureSocket, AHelloFromAnotherAddressDoesNotEndTheConnection)
{
	Rig rig;
	uint16 front = NextPort();
	Tap tap(front, rig.ServerPort);
	Core::addr_t serverAddress = {};
	auto device = rig.MakeDevice(rig.Camera, &serverAddress, front);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "one"), &received, &from, &tap));
	tap.ToServer.clear();

	// Somebody replays a hello, which was recorded (for the same address): the connection, which exists, keeps working.
	AuthHelloMessage hello = {};
	hello.Header.Version = CAM_PROTOCOL_VERSION;
	hello.Header.Type = MessageType::AUTH_HELLO;
	hello.KeyId = Core::DeviceKeyId(rig.Camera.Key);
	REQUIRE(Core::crypto::RandomBytes(hello.ClientRandom, AUTH_RANDOM_BYTES));
	std::vector<Byte> forged((Byte *)&hello, (Byte *)&hello + sizeof(hello));
	tap.InjectToServer(forged);
	tap.Pump();

	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "two"), &received, &from, &tap));
	CHECK_EQ(TextOf(received), "two");
}

TEST(SecureSocket, TheDeviceConnectsAgainAfterAServerRestart)
{
	Rig rig;
	Core::addr_t serverAddress = {};
	auto device = rig.MakeDevice(rig.Camera, &serverAddress);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "before"), &received, &from));
	std::vector<Byte> answer = AppMessage(MessageType::SERVER_CONNECTION_START, "ok");
	rig.Server->Send(answer.data(), (int32)answer.size(), from);
	std::vector<Byte> got;
	REQUIRE(Receive(device.get(), &got));
	REQUIRE(device->IsEstablished());

	// The server was restarted, it does not know the connection anymore.
	rig.StartServer();
	REQUIRE_EQ(rig.Server->PeerCount(), 0u);

	// The device keeps sending (it does not know), gets the reset, and makes a new connection on its own.
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "after"), &received, &from, nullptr, 6000));
	CHECK_EQ(TextOf(received), "after");
	CHECK_EQ(rig.Server->PeerCount(), 1u);
}

TEST(SecureSocket, ARemovedDeviceIsDisconnected)
{
	Rig rig;
	Core::addr_t serverAddress = {};
	auto device = rig.MakeDevice(rig.Camera, &serverAddress);

	std::vector<Byte> received;
	Core::addr_t from = {};
	REQUIRE(Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "one"), &received, &from));
	REQUIRE_EQ(rig.Server->PeerCount(), 1u);

	// The device is taken out of the list (a stolen camera, for example). The server notices that on its own, while it runs.
	std::string error;
	REQUIRE(rig.Registry.Remove("Front door", &error));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	rig.Server->UpdateDevices();

	CHECK_EQ(rig.Server->PeerCount(), 0u);

	// It can not connect again either: its key is not known anymore.
	CHECK(!Deliver(rig, device.get(), serverAddress, AppMessage(MessageType::CLIENT_HEARTBEAT, "two"), &received, &from, nullptr, 1500));
}

TEST(SecureSocket, TheServerDoesNotAnswerWithoutAKeyToDevices)
{
	Rig rig;

	// A hello with an unknown key id gets no answer at all (nothing to work with for somebody, who probes the server).
	std::unique_ptr<Core::Socket> probe(Core::Socket::Create());
	REQUIRE(probe->Open(true, "127.0.0.1", rig.ServerPort));
	REQUIRE(probe->SetNonBlocking(true));
	Core::addr_t server = probe->Lookup("127.0.0.1", rig.ServerPort);

	AuthHelloMessage hello = {};
	hello.Header.Version = CAM_PROTOCOL_VERSION;
	hello.Header.Type = MessageType::AUTH_HELLO;
	hello.KeyId = 0x1234567890abcdefull;
	probe->Send(&hello, sizeof(hello), server);

	std::vector<Byte> buffer(1024);
	int64 start = NowMS();
	while (NowMS() - start < 300)
	{
		std::vector<Byte> ignored(1);
		rig.Server->Recv(ignored.data(), 1, &server);

		Core::addr_t from = {};
		CHECK_EQ(probe->Recv(buffer.data(), (int32)buffer.size(), &from), 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}
