#pragma once

#include "Core/Core.h"

// The messages between the camera clients, the displays and the server. Every program includes this file through its own Messages.h.
//
// Who sends what:
//   camera client -> server : CLIENT_CONNECTION_START, CLIENT_FRAME (the frames), CLIENT_HEARTBEAT, CLIENT_CONNECTION_CLOSE
//   display       -> server : DISPLAY_CONNECTION_START, CLIENT_HEARTBEAT, CLIENT_CONNECTION_CLOSE
//   server -> camera client : SERVER_CONNECTION_START, SERVER_CONNECTION_CLOSE, SERVER_CLIENT_UNKNOWN
//   server -> display       : SERVER_DISPLAY_START, SERVER_FRAME_CHUNK (the frames of the cameras), SERVER_CONNECTION_CLOSE, SERVER_CLIENT_UNKNOWN

enum MessageType : uint16
{
	NONE = 0,
	CLIENT_CONNECTION_START,
	CLIENT_CONNECTION_CLOSE,
	CLIENT_FRAME,
	SERVER_CONNECTION_START,
	SERVER_CONNECTION_CLOSE,
	CLIENT_HEARTBEAT,
	SERVER_CLIENT_UNKNOWN,
	DISPLAY_CONNECTION_START,
	SERVER_DISPLAY_START,
	SERVER_FRAME_CHUNK
};

// The version of the protocol, every message carries it. It only changes, if the messages change in a way, which old programs do not understand.
// It is independent of CAM_VERSION (the version of the camera client, which is used to ship updates).
constexpr uint16 CAM_PROTOCOL_VERSION = 100;

#pragma pack(push, 1)

// Maximum number of payload bytes per frame datagram. Together with the chunk header this stays below a typical 1500 byte MTU,
// so no IP fragmentation happens (losing one fragment would lose the whole datagram).
constexpr uint32 FRAME_CHUNK_PAYLOAD_SIZE = 1200;

// Upper bound for one encoded frame, used to reject malformed chunk headers.
constexpr uint32 MAX_ENCODED_FRAME_SIZE = 8 * 1024 * 1024;

// Maximum length of the camera and display names, including the terminating zero.
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

// Sent regularly by a connected client or display, so the server can tell an idle one from a dead one.
struct ClientHeartbeatMessage
{
	header_t Header;
};

// Sent by the server to a client or display it does not know (anymore), e.g. after a timeout or a server restart. It has to connect again.
struct ServerClientUnknownMessage
{
	header_t Header;
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

// A display announces itself to the server, instead of sending frames it receives the frames of the cameras.
struct DisplayConnectionStartMessage
{
	header_t Header;
	char DisplayName[MAX_FRAME_NAME_LENGTH];

	// The name of the one camera, which this display wants to see. Empty for all cameras.
	char CameraFilter[MAX_FRAME_NAME_LENGTH];

	// The maximum number of frames per second the display wants to receive from every camera (0: the server decides).
	uint32 MaxFPS;
};

struct ServerDisplayStartResponse
{
	header_t Header;
	bool Accepted;
};

// The frames of the cameras are sent to the displays in this form. Same idea as ClientFrameChunkMessage (self-contained datagrams, incomplete frames
// are dropped), plus the camera, to which the frame belongs.
struct ServerFrameChunkMessage
{
	header_t Header;
	uint32 CameraId;						// Identifies the camera, as long as it is connected.
	char CameraName[MAX_FRAME_NAME_LENGTH];
	uint32 FrameId;							// Increases by one for every frame of this camera, wraps around.
	uint32 FrameSize;						// Total size of the JPEG encoded frame in bytes.
	uint16 ChunkIndex;
	uint16 ChunkCount;
};

#pragma pack(pop)
