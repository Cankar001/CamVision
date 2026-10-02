#pragma once

#include <Cam-Core.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Camera.h"
#include "Messages.h"

struct ClientConfig
{
	/// <summary>
	/// The address and port of the server, to which the frames get sent.
	/// </summary>
	std::string ServerIP = "127.0.0.1";
	uint16 Port = 45645;

	/// <summary>
	/// The name of this camera, used by the server to title the preview window (max. 31 characters).
	/// </summary>
	std::string Name = "Client #1";

	/// <summary>
	/// The frames per second announced to the server, used to size its backup buffer.
	/// </summary>
	uint32 FPS = 30;

	/// <summary>
	/// The JPEG quality (1 - 100) used to compress the frames before sending them.
	/// </summary>
	int32 JpegQuality = 80;

	/// <summary>
	/// Bandwidth tuning: Width in pixels, to which the frames are scaled down before sending (keeps the aspect ratio).
	/// 0 sends the frames in the size of the camera. Never scales up. The local preview is not affected.
	/// </summary>
	uint32 SendWidth = 0;

	/// <summary>
	/// Bandwidth tuning: Maximum number of frames per second, which are sent to the server. 0 sends every frame.
	/// The local preview is not affected.
	/// </summary>
	uint32 MaxFPS = 0;

	CameraConfig Camera;
};

class Client
{
public:

	Client(const ClientConfig &config);
	~Client();

	/// <summary>
	/// Frees all camera resources and joins all worker threads.
	/// </summary>
	void Release();

	/// <summary>
	/// Tells, if the client is currently running.
	/// </summary>
	/// <returns>Returns true, if all worker threads are running.</returns>
	bool IsRunning() const { return m_Running; }

	/// <summary>
	/// Runs the client, is responsible to start all worker threads.
	/// </summary>
	/// <param name="shouldShowFrames">Determines, if the main thread should show all current camera frames. If false, the frames are only sent (headless).</param>
	void Run(bool shouldShowFrames = true);

	/// <summary>
	/// Requests the client to stop. Safe to call from another thread or a signal handler, Run() returns after the connection was closed.
	/// </summary>
	void Stop();

	/// <summary>
	/// Handles the communication with the server.
	/// </summary>
	void NetworkLoop();

	/// <summary>
	/// Handles receiving the frames from the connected camera.
	/// </summary>
	void CameraLoop();

	/// <summary>
	/// Shows all current frames of the camera in a window and sends them to the server.
	/// </summary>
	void Show();

	/// <summary>
	/// Sends all current frames of the camera to the server without showing a window (headless, e.g. on a Raspberry Pi without display).
	/// </summary>
	void Stream();

private:

	/// <summary>
	/// Handles the connection begin response from the server.
	/// </summary>
	/// <param name="message">The message received from the server.</param>
	/// <param name="length">The length of the message in bytes.</param>
	/// <returns>Returns true, if the connection has been successfully confirmed.</returns>
	bool OnConnectionAccepted(Byte *message, uint32 length);

	/// <summary>
	/// Handles the connection close response from the server.
	/// </summary>
	/// <param name="message">The message received from the server.</param>
	/// <param name="length">The length of the message in bytes.</param>
	/// <returns>Returns true, if the connection has been successfully confirmed to be closed.</returns>
	bool OnConnectionClosed(Byte *message, uint32 length);

	/// <summary>
	/// Processes the frame data (image analytics).
	/// </summary>
	/// <param name="frame">The frame to analyze.</param>
	/// <param name="frame_size">The size of the frame in bytes.</param>
	/// <param name="frame_width">The frame width.</param>
	/// <param name="frame_height">The frame height.</param>
	void ProcessFrame(Byte *frame, uint32 frame_size, uint32 frame_width, uint32 frame_height);

	/// <summary>
	/// Sends the provided frame data to the connected server. The frame is JPEG encoded and sent as a series of independent datagrams (fire and forget).
	/// </summary>
	/// <param name="frame">The frame to send to the server.</param>
	/// <param name="frame_size">The size of the frame in bytes.</param>
	/// <param name="frame_width">The frame width.</param>
	/// <param name="frame_height">The frame height.</param>
	void SendFrameToServer(Byte *frame, uint32 frame_size, uint32 frame_width, uint32 frame_height);

private:

	ClientConfig m_Config;
	Core::Socket *m_Socket = nullptr;
	Core::addr_t m_Host;
	
	uint32 m_Version;
	std::atomic<bool> m_Running = true;
	std::atomic<bool> m_NetworkThreadFinished = false;
	bool m_SentConnectionCloseRequest = false;
	std::atomic<bool> m_ConnectedToServer = false;
	uint32 m_NextFrameId = 0;
	std::chrono::steady_clock::time_point m_LastFrameSent;
	std::vector<uchar> m_EncodeBuffer;
	cv::Mat m_ScaledImage;
	Camera m_Camera;

	std::thread m_NetworkThread;
	std::thread m_CameraThread;
};