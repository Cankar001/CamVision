#pragma once

#include <Cam-Core.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "Messages.h"

struct DisplayClientConfig
{
	/// <summary>
	/// The address and port of the server, to which the display connects.
	/// </summary>
	std::string ServerIP = "127.0.0.1";
	uint16 Port = 45645;

	/// <summary>
	/// The name of this display, shown in the log of the server.
	/// </summary>
	std::string Name = "Display #1";

	/// <summary>
	/// The key of this device (64 hex characters), which the server made with CamServer --add_device. With it, the connection to the server is
	/// authenticated and encrypted. Without it, the messages are sent as they are, which only works with a server, which has the authentication turned off.
	/// </summary>
	std::string Key;

	/// <summary>
	/// The name of the one camera, which this display shows (fullscreen). Empty shows all cameras next to each other.
	/// </summary>
	std::string Camera;

	/// <summary>
	/// The maximum number of frames per second the display wants to receive from every camera. A weak device or a slow network needs fewer.
	/// </summary>
	uint32 MaxFPS = 10;

	/// <summary>
	/// The size of the picture in the test mode (SnapshotFile). The display itself always covers the whole screen.
	/// </summary>
	uint32 WindowWidth = 1280;
	uint32 WindowHeight = 720;

	/// <summary>
	/// Test mode: if set, no window is opened. The first picture with a received camera is stored in this file, then the program quits.
	/// </summary>
	std::string SnapshotFile;

	/// <summary>
	/// How long the test mode waits for the cameras.
	/// </summary>
	uint32 SnapshotTimeoutSeconds = 30;

	/// <summary>
	/// The test mode waits until this many cameras sent a picture, before it stores the picture.
	/// </summary>
	uint32 SnapshotMinCameras = 1;
};

/// <summary>
/// A display for the security cameras. It connects to the server like a camera client does, but tells it that it is a display: instead of sending frames,
/// it receives the frames of the cameras (all of them, or just one) from the server, and shows them. Many displays can be connected at the same time,
/// each one can be at a different place.
/// </summary>
class DisplayClient
{
public:

	DisplayClient(const DisplayClientConfig &config);
	~DisplayClient();

	/// <summary>
	/// Shows the cameras, until Esc (or Q) is pressed, the window is closed, or Stop() is called.
	/// </summary>
	/// <returns>Returns the exit code of the program (0: everything is fine, 1: the test mode did not receive a picture).</returns>
	int Run();

	/// <summary>
	/// Makes Run() return. Safe to call from a signal handler.
	/// </summary>
	void Stop();

private:

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

	/// <summary>
	/// What is known about a camera (all this is written by the network thread, and read by the thread, which shows the pictures).
	/// </summary>
	struct CameraFeed
	{
		std::string Name;
		FrameAssembly Assembly;
		EncodedFrame Latest;
		uint32 LatestNumber = 0;
		int64 LastFrameMS = 0;
	};

	/// <summary>
	/// A decoded picture of a camera, kept by the thread, which shows the pictures.
	/// </summary>
	struct DecodedCamera
	{
		std::string Name;
		cv::Mat Image;
		uint32 Number = 0;
		int64 LastFrameMS = 0;
	};

	void NetworkLoop();
	void OnFrameChunk(Byte *message, int32 length);

	/// <summary>
	/// Draws everything the display shows into a picture of the given size.
	/// </summary>
	/// <param name="fresh_cameras">Is set to the number of cameras, which sent a picture lately.</param>
	cv::Mat Compose(int width, int height, uint32 *fresh_cameras);

private:

	DisplayClientConfig m_Config;
	Core::Socket *m_Socket = nullptr;
	Core::addr_t m_Host = {};

	std::atomic<bool> m_Running{ false };
	std::atomic<bool> m_Connected{ false };
	std::thread m_NetworkThread;

	// The feeds of the cameras, by camera id.
	std::mutex m_FeedsMutex;
	std::map<uint32, CameraFeed> m_Feeds;

	// Only used by the thread, which shows the pictures.
	std::map<uint32, DecodedCamera> m_Decoded;
};
