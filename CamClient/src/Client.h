#pragma once

#include <Cam-Core.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "Camera.h"
#include "Messages.h"

struct ClientConfig
{
	std::string ServerIP;
	uint16 Port;
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
	/// <param name="shouldShowFrames">Determines, if the main thread should show all current camera frames.</param>
	void Run(bool shouldShowFrames = true);

	/// <summary>
	/// Handles the communication with the server.
	/// </summary>
	void NetworkLoop();

	/// <summary>
	/// Handles receiving the frames from the connected camera.
	/// </summary>
	void CameraLoop();

	/// <summary>
	/// Shows all current frames of the camera.
	/// </summary>
	void Show();

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
	bool m_NetworkThreadFinished = false;
	bool m_SentConnectionCloseRequest = false;
	std::atomic<bool> m_ConnectedToServer = false;
	uint32 m_NextFrameId = 0;
	std::vector<uchar> m_EncodeBuffer;
	Camera m_Camera;

	std::thread m_NetworkThread;
	std::thread m_CameraThread;
};