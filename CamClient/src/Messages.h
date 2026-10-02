#pragma once

#include <Cam-Core.h>

enum MessageType : uint16
{
	NONE = 0,
	CLIENT_CONNECTION_START,
	CLIENT_CONNECTION_CLOSE,
	CLIENT_FRAME,
	SERVER_CONNECTION_START,
	SERVER_CONNECTION_CLOSE
};

#pragma pack(push, 1)

// Maximum number of payload bytes per frame datagram. Together with the chunk header this stays below a typical 1500 byte MTU,
// so no IP fragmentation happens (losing one fragment would lose the whole datagram).
constexpr uint32 FRAME_CHUNK_PAYLOAD_SIZE = 1200;

// Upper bound for one encoded frame, used to reject malformed chunk headers.
constexpr uint32 MAX_ENCODED_FRAME_SIZE = 8 * 1024 * 1024;

// Maximum length of the camera name, including the terminating zero.
constexpr uint32 MAX_FRAME_NAME_LENGTH = 32;

struct header_t
{
	uint16 Version;
	uint16 Type;
};

struct ClientConnectionStartMessage
{
	header_t Header;
	char FrameName[MAX_FRAME_NAME_LENGTH];
	uint32 FPS;
};

struct ClientConnectionCloseMessage
{
	header_t Header;

};

// Every datagram of a frame carries this header followed by up to FRAME_CHUNK_PAYLOAD_SIZE bytes of JPEG data.
// Datagrams are self-contained, so the server can reassemble frames without any additional state from the client.
// Incomplete frames are dropped, never retransmitted.
struct ClientFrameChunkMessage
{
	header_t Header;
	uint32 FrameId;			// Increases by one for every frame, wraps around.
	uint32 FrameSize;		// Total size of the JPEG encoded frame in bytes.
	uint16 ChunkIndex;
	uint16 ChunkCount;
};

struct ServerConnectionStartResponse
{
	header_t Header;
	bool ConnectionAccepted;
};

struct ServerConnectionCloseResponse
{
	header_t Header;
	bool ConnectionClosed;
};

#pragma pack(pop)

