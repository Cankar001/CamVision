#pragma once

#include <Cam-Core.h>

enum MessageType : uint16
{
	NONE = 0,
	CLIENT_REQUEST_VERSION,
	CLIENT_UPDATE_BEGIN,
	CLIENT_UPDATE_PIECE,
	SERVER_RECEIVE_VERSION,
	SERVER_UPDATE_TOKEN,
	SERVER_UPDATE_BEGIN,
	SERVER_UPDATE_PIECE,

	// The update was replaced on the server, while the client downloaded it. Added at the end, so old programs, which do not know it, ignore it.
	SERVER_UPDATE_CHANGED
};

#define SIG_BYTES 512
#define PIECE_BYTES 1024

struct Signature
{
	Byte Data[SIG_BYTES];
};

#pragma pack(push, 1)

struct header_t
{
	uint16 Version;
	uint16 Type;
};

/// <summary>
/// This message is sent, if the client wants to know the current server version.
/// Corresponds to CLIENT_REQUEST_VERSION message type.
/// </summary>
struct ClientWantsVersionMessage
{
	header_t Header;
	uint32 LocalVersion;
	uint32 ClientVersion;
};

struct ClientUpdateBeginMessage
{
	header_t Header;
	uint64 ClientToken;
	uint64 ServerToken;
	uint32 ClientVersion;
};

struct ClientUpdatePieceMessage
{
	header_t Header;
	uint64 ClientToken;
	uint64 ServerToken;
	uint32 PiecePos;
};

struct ServerVersionInfoMessage
{
	header_t Header;
	uint32 Version;
	Core::Crypto::key_t PublicKey;
};

struct ServerUpdateBeginMessage
{
	header_t Header;
	uint64 ClientToken;
	uint64 ServerToken;
	uint32 UpdateSize;
	Signature UpdateSignature;
};

struct ServerUpdateTokenMessage
{
	header_t Header;
	uint64 ClientToken;
	uint64 ServerToken;
};

struct ServerUpdatePieceMessage
{
	header_t Header;
	uint64 ClientToken;
	uint64 ServerToken;
	uint32 PiecePos;
	uint16 PieceSize;
};

/// <summary>
/// The server answers a request for a piece with this message, if the update, which the client began to download, is not the current one anymore (the server
/// built a new one in the meantime). The pieces of the new update do not fit to the ones already received, so the client starts again with the new version.
/// </summary>
struct ServerUpdateChangedMessage
{
	header_t Header;
	uint64 ClientToken;
	uint64 ServerToken;
};

#pragma pack(pop)

