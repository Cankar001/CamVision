#include "CamTest.h"

#include "Net/CamMessages.h"

#include <cstring>

// The messages are the contract between programs of different versions on different machines (a Raspberry Pi and a PC), so the layout is pinned here.
// If one of these fails, the protocol changed: change CAM_PROTOCOL_VERSION too, and update the numbers here on purpose.

static_assert(sizeof(header_t) == 4, "header_t must have no padding");
static_assert(sizeof(ClientFrameChunkMessage) == sizeof(header_t) + 4 + 4 + 2 + 2, "chunk header must have no padding");
static_assert(sizeof(ServerFrameChunkMessage) == sizeof(header_t) + 4 + MAX_FRAME_NAME_LENGTH + 4 + 4 + 2 + 2, "chunk header must have no padding");
static_assert(sizeof(ClientConnectionStartMessage) == sizeof(header_t) + MAX_FRAME_NAME_LENGTH + 4, "no padding");
static_assert(sizeof(DisplayConnectionStartMessage) == sizeof(header_t) + 2 * MAX_FRAME_NAME_LENGTH + 4, "no padding");
static_assert(sizeof(AuthHelloMessage) == sizeof(header_t) + 8 + AUTH_RANDOM_BYTES, "no padding");
static_assert(sizeof(AuthChallengeMessage) == sizeof(header_t) + 2 * AUTH_RANDOM_BYTES, "no padding");
static_assert(sizeof(SecureDataHeader) == sizeof(header_t) + 8, "no padding");

TEST(Protocol, MessageTypeValuesAreStable)
{
	CHECK_EQ((int)MessageType::NONE, 0);
	CHECK_EQ((int)MessageType::CLIENT_CONNECTION_START, 1);
	CHECK_EQ((int)MessageType::CLIENT_CONNECTION_CLOSE, 2);
	CHECK_EQ((int)MessageType::CLIENT_FRAME, 3);
	CHECK_EQ((int)MessageType::SERVER_CONNECTION_START, 4);
	CHECK_EQ((int)MessageType::SERVER_CONNECTION_CLOSE, 5);
	CHECK_EQ((int)MessageType::CLIENT_HEARTBEAT, 6);
	CHECK_EQ((int)MessageType::SERVER_CLIENT_UNKNOWN, 7);
	CHECK_EQ((int)MessageType::DISPLAY_CONNECTION_START, 8);
	CHECK_EQ((int)MessageType::SERVER_DISPLAY_START, 9);
	CHECK_EQ((int)MessageType::SERVER_FRAME_CHUNK, 10);
	CHECK_EQ((int)MessageType::AUTH_HELLO, 11);
	CHECK_EQ((int)MessageType::AUTH_CHALLENGE, 12);
	CHECK_EQ((int)MessageType::SECURE_DATA, 13);
	CHECK_EQ((int)MessageType::AUTH_RESET, 14);
}

TEST(Protocol, ChunksFitIntoOneEthernetFrame)
{
	// A datagram, which does not fit into the MTU of 1500 bytes, gets fragmented, and losing one fragment loses the whole datagram.
	const uint32 ip_and_udp_header = 20 + 8;
	CHECK((uint32)sizeof(ServerFrameChunkMessage) + FRAME_CHUNK_PAYLOAD_SIZE + ip_and_udp_header <= 1500u);
	CHECK((uint32)sizeof(ClientFrameChunkMessage) + FRAME_CHUNK_PAYLOAD_SIZE + ip_and_udp_header <= 1500u);
}

TEST(Protocol, SecureChunksStillFitIntoOneEthernetFrame)
{
	// Sealed messages are larger by the counter header and the tag of the encryption (28 bytes): they must not be fragmented either.
	const uint32 ip_and_udp_header = 20 + 8;
	const uint32 secure_overhead = (uint32)sizeof(SecureDataHeader) + 16;
	CHECK((uint32)sizeof(ServerFrameChunkMessage) + FRAME_CHUNK_PAYLOAD_SIZE + secure_overhead + ip_and_udp_header <= 1500u);
}

TEST(Protocol, AFrameOfTheMaximumSizeNeedsLessChunksThanTheCounterHolds)
{
	uint32 chunks = (MAX_ENCODED_FRAME_SIZE + FRAME_CHUNK_PAYLOAD_SIZE - 1) / FRAME_CHUNK_PAYLOAD_SIZE;
	CHECK(chunks <= 0xFFFFu);
}

TEST(Protocol, HeaderIsFourBytesInNetworkOrderOfTheMembers)
{
	header_t header = {};
	header.Version = CAM_PROTOCOL_VERSION;
	header.Type = MessageType::CLIENT_HEARTBEAT;

	const unsigned char *bytes = (const unsigned char *)&header;
	uint16 version = 0, type = 0;
	memcpy(&version, bytes, 2);
	memcpy(&type, bytes + 2, 2);
	CHECK_EQ(version, CAM_PROTOCOL_VERSION);
	CHECK_EQ(type, (uint16)MessageType::CLIENT_HEARTBEAT);
}

TEST(Protocol, TheProtocolVersionIsSet)
{
	CHECK(CAM_PROTOCOL_VERSION >= 100);
}
