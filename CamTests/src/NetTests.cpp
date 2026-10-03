#include "CamTest.h"
#include "TestUtils.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

// Receives one datagram within the timeout (the sockets are non-blocking).
static int32 ReceiveWithin(Core::Socket *socket, void *buffer, int32 size, Core::addr_t *from, int timeout_ms)
{
	auto start = std::chrono::steady_clock::now();
	while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < timeout_ms)
	{
		int32 length = socket->Recv(buffer, size, from);
		if (length != 0)
		{
			return length;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	return 0;
}

TEST(Socket, SendsAndReceivesOnLoopback)
{
	std::unique_ptr<Core::Socket> server(Core::Socket::Create());
	REQUIRE(server->Open());
	REQUIRE(server->BindLoopback(45871));
	REQUIRE(server->SetNonBlocking(true));

	std::unique_ptr<Core::Socket> client(Core::Socket::Create());
	REQUIRE(client->Open(true, "127.0.0.1", 45871));
	REQUIRE(client->SetNonBlocking(true));

	Core::addr_t target = client->Lookup("127.0.0.1", 45871);
	const char message[] = "hello server";
	CHECK_EQ(client->Send(message, (int32)sizeof(message), target), (int32)sizeof(message));

	char buffer[256] = {};
	Core::addr_t from = {};
	int32 length = ReceiveWithin(server.get(), buffer, sizeof(buffer), &from, 2000);
	REQUIRE_EQ(length, (int32)sizeof(message));
	CHECK(strcmp(buffer, message) == 0);

	// The server can answer to the sender.
	const char answer[] = "hello client";
	CHECK_EQ(server->Send(answer, (int32)sizeof(answer), from), (int32)sizeof(answer));

	Core::addr_t answer_from = {};
	length = ReceiveWithin(client.get(), buffer, sizeof(buffer), &answer_from, 2000);
	REQUIRE_EQ(length, (int32)sizeof(answer));
	CHECK(strcmp(buffer, answer) == 0);
	CHECK(answer_from.Value == target.Value);
}

TEST(Socket, ReceiveReturnsZeroWhenNothingArrived)
{
	// Returning 0 (and not an error) is what the receive loops of all programs rely on.
	std::unique_ptr<Core::Socket> socket(Core::Socket::Create());
	REQUIRE(socket->Open());
	REQUIRE(socket->BindLoopback(45872));
	REQUIRE(socket->SetNonBlocking(true));

	char buffer[16];
	Core::addr_t from = {};
	CHECK_EQ(socket->Recv(buffer, sizeof(buffer), &from), 0);
}

TEST(Socket, LookupIsStableAndKeepsThePort)
{
	std::unique_ptr<Core::Socket> socket(Core::Socket::Create());
	Core::addr_t a = socket->Lookup("127.0.0.1", 1234);
	Core::addr_t b = socket->Lookup("127.0.0.1", 1234);
	Core::addr_t c = socket->Lookup("127.0.0.1", 1235);
	CHECK(a.Value == b.Value);
	CHECK(a.Value != c.Value);
	CHECK_EQ(a.Host, c.Host);
}

TEST(Socket, SetsBufferSizes)
{
	std::unique_ptr<Core::Socket> socket(Core::Socket::Create());
	REQUIRE(socket->Open());
	CHECK(socket->SetBufferSizes(256 * 1024, 1024 * 1024));
}

TEST(Socket, DatagramsKeepTheirBoundaries)
{
	// UDP: two sends are two receives, never glued together (the frame chunks depend on it).
	std::unique_ptr<Core::Socket> server(Core::Socket::Create());
	REQUIRE(server->Open());
	REQUIRE(server->BindLoopback(45873));
	REQUIRE(server->SetNonBlocking(true));

	std::unique_ptr<Core::Socket> client(Core::Socket::Create());
	REQUIRE(client->Open(true, "127.0.0.1", 45873));
	Core::addr_t target = client->Lookup("127.0.0.1", 45873);

	char first[100], second[1200];
	memset(first, 1, sizeof(first));
	memset(second, 2, sizeof(second));
	client->Send(first, sizeof(first), target);
	client->Send(second, sizeof(second), target);

	char buffer[2048];
	Core::addr_t from = {};
	CHECK_EQ(ReceiveWithin(server.get(), buffer, sizeof(buffer), &from, 2000), (int32)sizeof(first));
	CHECK_EQ(ReceiveWithin(server.get(), buffer, sizeof(buffer), &from, 2000), (int32)sizeof(second));
}
