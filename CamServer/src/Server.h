#pragma once

#include <Cam-Core.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>

#include <opencv2/opencv.hpp>

struct ServerConfig
{
	/// <summary>
	/// The IP address of the server.
	/// </summary>
	std::string ServerIP;
	
	/// <summary>
	/// The port, at which the server should listen.
	/// </summary>
	uint16 Port;

	/// <summary>
	/// The duration in minutes of each camera feed to be kept in memory for saving to disk after something happened.
	/// </summary>
	uint32 VideoBackupDuration;
};

/// <summary>
/// An encoded (JPEG) frame. Shared, so the ring buffer, the preview and the frame assembly never copy the pixel data.
/// </summary>
using EncodedFrame = std::shared_ptr<std::vector<uchar>>;

/// <summary>
/// The frame, which is currently being reassembled from its datagrams.
/// </summary>
struct FrameAssembly
{
	uint32 FrameId = 0;
	uint32 FrameSize = 0;
	uint32 ChunkCount = 0;
	uint32 ReceivedChunks = 0;
	bool Active = false;
	std::vector<bool> ReceivedMask;
	EncodedFrame Data;
};

struct ClientEntry
{
	Core::addr_t Address;

	/// <summary>
	/// The last X minutes of encoded frames, kept in memory for saving to disk after something happened.
	/// </summary>
	Core::RingBuffer<EncodedFrame> Frames;
	std::string FrameTitle;

	FrameAssembly Assembly;

	/// <summary>
	/// The newest complete frame, used by the live preview.
	/// </summary>
	EncodedFrame LatestFrame;
	uint32 LatestFrameNumber = 0;

	ClientEntry(uint32 frame_capacity)
		: Frames(frame_capacity)
	{
		Address = {};
	}

	// The ring buffer owns raw memory, so entries must never be copied.
	ClientEntry(const ClientEntry &) = delete;
	ClientEntry &operator=(const ClientEntry &) = delete;
};

class Server
{
public:

	Server(const ServerConfig &config);
	~Server();

	void Run();
	void StartFramePreviews();

private:

	bool Step();

	bool OnClientConnected(Core::addr_t &clientAddr, Byte *message, int32 addrLen);
	bool OnClientDisconnected(Core::addr_t &clientAddr, Byte *message, int32 addrLen);
	bool OnClientFrameChunk(Core::addr_t &clientAddr, Byte *message, int32 addrLen);

	/// <summary>
	/// Must be called with m_ClientsMutex locked.
	/// </summary>
	ClientEntry *FindClient(const Core::addr_t &clientAddr);

	void FramePreview();

private:

	ServerConfig m_Config;
	Core::Socket *m_Socket = nullptr;

	uint32 m_Version;
	bool m_Running = true;

	// Guards m_Clients and everything inside the entries, because the preview thread reads them while the network thread writes them.
	std::mutex m_ClientsMutex;
	std::vector<std::unique_ptr<ClientEntry>> m_Clients;
	std::thread m_FramePreviewThread;
};

