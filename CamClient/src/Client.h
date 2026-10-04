#pragma once

#include <Cam-Core.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Features.h"
#include "Camera.h"

#if FRAME_ANALYSIS // FRAME ANALYSIS (movement and face detection in the camera client): switch all blocks with this tag to "#if 1" to enable it
#include "MotionDetector.h"
#endif // FRAME_ANALYSIS
#include "Messages.h"

#if FRAME_ANALYSIS // FRAME ANALYSIS (movement and face detection in the camera client): switch all blocks with this tag to "#if 1" to enable it
/// <summary>
/// Analyzes every frame in the camera client, and sends frames only if something happens (this saves a lot of bandwidth on a Raspberry Pi).
/// While there is movement (or a face), and for a while after it, all frames are sent. Without it, only a few frames per second are sent, so the
/// displays and the server do not show a frozen picture.
/// </summary>
struct FrameAnalysisConfig
{
	bool Enabled = false;

	/// <summary>
	/// Detect movement.
	/// </summary>
	bool DetectMotion = true;

	/// <summary>
	/// How much of the picture (in percent) has to change, to count as movement, and how much a pixel has to change (0 - 255).
	/// </summary>
	float MotionMinArea = 1.0f;
	int32 MotionPixelThreshold = 25;

	/// <summary>
	/// Detect faces (not who it is, that is done by the server). Needs the YuNet model (the same file as the server uses), and OpenCV 4.5.4 or newer.
	/// A face counts as activity like movement does (a person, who stands still in front of the camera, is not moving).
	/// </summary>
	bool DetectFaces = false;
	std::string FaceModel = "models/face_detection_yunet_2023mar.onnx";

	/// <summary>
	/// The faces are searched in every N-th frame only, because it takes long.
	/// </summary>
	uint32 FaceEveryNFrames = 10;

	/// <summary>
	/// How long all frames are sent after the last movement (or face).
	/// </summary>
	uint32 HoldSeconds = 10;

	/// <summary>
	/// The frames per second, which are sent while nothing happens.
	/// </summary>
	uint32 IdleFPS = 1;
};
#endif // FRAME_ANALYSIS

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
	/// The key of this device (64 hex characters), which the server made with CamServer --add_device. With it, the connection to the server is
	/// authenticated and encrypted. Without it, the messages are sent as they are, which only works with a server, which has the authentication turned off.
	/// </summary>
	std::string Key;

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

#if FRAME_ANALYSIS // FRAME ANALYSIS (movement and face detection in the camera client): switch all blocks with this tag to "#if 1" to enable it
	FrameAnalysisConfig Analysis;
#endif // FRAME_ANALYSIS
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

	// True, if the connection to the server is authenticated and encrypted (a device key is set).
	bool m_Secure = false;
	std::atomic<bool> m_NetworkThreadFinished = false;
	bool m_SentConnectionCloseRequest = false;
	std::atomic<bool> m_ConnectedToServer = false;
	uint32 m_NextFrameId = 0;
	std::chrono::steady_clock::time_point m_LastFrameSent;
	std::vector<uchar> m_EncodeBuffer;
	cv::Mat m_ScaledImage;

	// ProcessFrame decides, if the current frame is sent to the server. Frames are sent, unless the frame analysis says otherwise.
	bool m_SendThisFrame = true;

#if FRAME_ANALYSIS // FRAME ANALYSIS (movement and face detection in the camera client): switch all blocks with this tag to "#if 1" to enable it
	void InitAnalysis();

	std::unique_ptr<MotionDetector> m_MotionDetector;
	cv::Ptr<cv::FaceDetectorYN> m_FaceDetector;
	uint32 m_AnalyzedFrames = 0;
	int64 m_LastActivityMS = 0;
	int64 m_LastIdleSendMS = 0;
	bool m_WasActive = false;
#endif // FRAME_ANALYSIS
	Camera m_Camera;

	std::thread m_NetworkThread;
	std::thread m_CameraThread;
};