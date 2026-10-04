#pragma once

#include <Cam-Core.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <memory>
#include <atomic>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <opencv2/opencv.hpp>

#include "FaceAnalyzer.h"
#include "Mailer.h"
#include "Notifier.h"
#include "Recorder.h"

struct ServerConfig
{
	/// <summary>
	/// The IP address of the server (informational, the server listens on all interfaces).
	/// </summary>
	std::string ServerIP = "0.0.0.0";
	
	/// <summary>
	/// The port, at which the server should listen.
	/// </summary>
	uint16 Port = 45645;

	/// <summary>
	/// The duration in minutes of each camera feed to be kept in memory for saving to disk after something happened.
	/// </summary>
	uint32 VideoBackupDuration = 5;

	/// <summary>
	/// Determines, if the live preview windows of all connected cameras are shown.
	/// </summary>
	bool ShowPreview = true;

	/// <summary>
	/// The settings of the face detection and recognition.
	/// </summary>
	FaceConfig Faces;

	/// <summary>
	/// The settings of the recordings (schedule, folder, ...).
	/// </summary>
	RecorderConfig Recording;

	/// <summary>
	/// How many minutes are saved, if nothing else is said (the key R in the preview window).
	/// </summary>
	uint32 RecordDefaultMinutes = 5;

	/// <summary>
	/// Saves the last minutes of a camera automatically, when an unknown person is seen by it (needs the face recognition).
	/// </summary>
	bool RecordOnUnknownPerson = false;
	uint32 RecordEventMinutes = 1;

	/// <summary>
	/// The port of the control channel (only reachable from this computer), with which a running server is told to save recordings, to report its
	/// status, or to stop. 0 turns it off.
	/// </summary>
	uint16 ControlPort = 45650;

	/// <summary>
	/// The settings for the emails, which are sent when an unknown person is seen.
	/// </summary>
	EmailConfig Email;

	/// <summary>
	/// Seconds without any message after which a client is considered dead and removed (its frames are freed). 0 disables the timeout.
	/// </summary>
	uint32 ClientTimeoutSeconds = 15;

	/// <summary>
	/// Only devices (cameras and displays) with a key of the list of devices may connect, everything is encrypted. If this is off, everybody in the network
	/// can connect as a camera (and send pictures) or as a display (and see all cameras), and everything is sent as it is.
	/// </summary>
	bool RequireAuth = true;

	/// <summary>
	/// The file with the devices and their keys (see Core::DeviceRegistry, and CamServer --add_device).
	/// </summary>
	std::string DevicesFile = "devices.cfg";
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
	/// Identifies the camera for the displays, as long as it is connected.
	/// </summary>
	uint32 CameraId = 0;

	/// <summary>
	/// The last X minutes of encoded frames, kept in memory for saving to disk after something happened.
	/// </summary>
	Core::RingBuffer<RecordedFrame> Frames;
	std::string FrameTitle;

	FrameAssembly Assembly;

	/// <summary>
	/// The newest complete frame, used by the live preview.
	/// </summary>
	EncodedFrame LatestFrame;
	uint32 LatestFrameNumber = 0;

	/// <summary>
	/// The faces, which were found in the latest analyzed frame (used by the preview and the displays), and when this was.
	/// </summary>
	std::vector<FaceResult> Faces;
	int64 FacesUpdatedMS = 0;
	uint32 DroppedFrames = 0;

	/// <summary>
	/// The last time, something was received from this client (connect request, heartbeat or frame data).
	/// </summary>
	std::chrono::steady_clock::time_point LastSeen = std::chrono::steady_clock::now();

	ClientEntry(uint32 frame_capacity)
		: Frames(frame_capacity)
	{
		Address = {};
	}

	// The ring buffer owns raw memory, so entries must never be copied.
	ClientEntry(const ClientEntry &) = delete;
	ClientEntry &operator=(const ClientEntry &) = delete;
};

/// <summary>
/// A display, which is connected to the server. It receives the frames of the cameras instead of sending frames.
/// </summary>
struct DisplayEntry
{
	Core::addr_t Address;
	std::string Name;

	/// <summary>
	/// The camera this display wants to see (the name of the camera). Empty for all cameras.
	/// </summary>
	std::string CameraFilter;

	/// <summary>
	/// The maximum number of frames per second to send per camera.
	/// </summary>
	uint32 MaxFPS = 10;

	std::chrono::steady_clock::time_point LastSeen = std::chrono::steady_clock::now();

	/// <summary>
	/// The time, when the last frame of a camera (by id) was sent to this display.
	/// </summary>
	std::unordered_map<uint32, int64> LastSentMS;

	/// <summary>
	/// The number of the last frame of a camera (by id), which was sent to this display.
	/// </summary>
	std::unordered_map<uint32, uint32> LastSentNumber;
};

class Server
{
public:

	Server(const ServerConfig &config);
	~Server();

	void Run();
	void StartFramePreviews();

	/// <summary>
	/// Starts the face detection (and recognition) of all camera feeds. Does nothing, if it is turned off or not possible (the reason is logged).
	/// </summary>
	void StartFaceAnalysis();

	/// <summary>
	/// Starts the emails (unknown people, cameras going offline). Does nothing, if they are turned off or not possible (the reason is logged).
	/// Independent of the face analysis, but unknown people can only be reported, if it is running.
	/// </summary>
	void StartNotifications();

	/// <summary>
	/// Starts the recording (on schedule), the saving of the last minutes on demand, and the control channel.
	/// </summary>
	void StartRecording();

	/// <summary>
	/// Makes Run() return, and finishes all recordings. Safe to call from a signal handler.
	/// </summary>
	void Stop();

	/// <summary>
	/// Saves the last minutes of the frames of a camera (or of all cameras) as video files.
	/// </summary>
	/// <param name="minutes">How many minutes, at most as many as the buffer holds (VideoBackupDuration).</param>
	/// <param name="cameraFilter">The name of the camera, or empty for all cameras.</param>
	/// <param name="label">Part of the file name.</param>
	/// <param name="savedFiles">Receives the number of files.</param>
	/// <returns>A text for the user: the files, or what went wrong.</returns>
	std::string SaveLastMinutes(uint32 minutes, const std::string &cameraFilter, const std::string &label, uint32 *savedFiles);

private:

	bool Step();

	bool OnClientConnected(Core::addr_t &clientAddr, Byte *message, int32 addrLen);
	bool OnClientDisconnected(Core::addr_t &clientAddr, Byte *message, int32 addrLen);
	bool OnClientHeartbeat(Core::addr_t &clientAddr, Byte *message, int32 addrLen);
	bool OnDisplayConnected(Core::addr_t &displayAddr, Byte *message, int32 addrLen);

	/// <summary>
	/// Sends the new frames of the cameras to the displays, which want to see them, on a thread of its own. If the face analysis is running, the faces
	/// are drawn into the frames first.
	/// </summary>
	void ForwardLoop();

	/// <summary>
	/// Sends one frame of a camera to a display (in chunks, like the frames of the cameras).
	/// </summary>
	void SendFrameToDisplay(const Core::addr_t &display, uint32 cameraId, const std::string &cameraName, uint32 frameNumber, const EncodedFrame &frame);
	bool OnClientFrameChunk(Core::addr_t &clientAddr, Byte *message, int32 addrLen);

	/// <summary>
	/// Must be called with m_ClientsMutex locked.
	/// </summary>
	ClientEntry *FindClient(const Core::addr_t &clientAddr);

	void FramePreview();

	/// <summary>
	/// Analyzes the latest frames of all cameras on a thread of its own, and reports the people who are seen.
	/// </summary>
	void FaceLoop();

	/// <summary>
	/// The control channel: commands of a program on this computer (CamServer --record_now, ...).
	/// </summary>
	void ControlLoop();

	/// <summary>
	/// Called, when an unknown person is seen by a camera (a face, which matches nobody of the known people), at most once per cooldown and camera.
	/// </summary>
	/// <param name="camera">The name of the camera.</param>
	/// <param name="face">The unknown face (position, detector score, best similarity).</param>
	/// <param name="snapshotFile">The photo with the marked face, or empty if no snapshot was stored (face_snapshots is off).</param>
	/// <param name="snapshotJpeg">The photo with the marked face (JPEG), for the attachment of the email. Empty, if there is none.</param>
	void NotifyUnknownPerson(const std::string &camera, const FaceResult &face, const std::string &snapshotFile, const std::vector<uchar> &snapshotJpeg);

	/// <summary>
	/// Removes all clients, which were not heard of for longer than the configured timeout.
	/// </summary>
	void ReapStaleClients();
	void SendClientUnknown(Core::addr_t &clientAddr);

private:

	ServerConfig m_Config;
	Core::Socket *m_Socket = nullptr;

	// The devices, which may connect, and the socket, which checks and encrypts everything (m_Socket is this socket, if the authentication is on).
	std::unique_ptr<Core::DeviceRegistry> m_Devices;
	Core::SecureSocket *m_Secure = nullptr;

	// When a device with the wrong key was last reported (by address), so a device, which keeps trying, does not flood the log.
	std::map<uint64, int64> m_RefusalWarnings;
	bool ShouldWarnAboutRefusal(Core::addr_t addr);

	uint32 m_Version;
	bool m_Running = true;

	// Guards m_Clients and everything inside the entries, because the preview thread reads them while the network thread writes them.
	std::mutex m_ClientsMutex;
	std::vector<std::unique_ptr<ClientEntry>> m_Clients;

	// The connected displays, guarded by the same mutex.
	std::vector<std::unique_ptr<DisplayEntry>> m_Displays;
	uint32 m_NextCameraId = 1;
	std::thread m_FramePreviewThread;
	std::thread m_FaceThread;
	std::unique_ptr<FaceAnalyzer> m_FaceAnalyzer;
	std::unique_ptr<Mailer> m_Mailer;
	std::unique_ptr<Notifier> m_Notifier;
	std::unique_ptr<Recorder> m_Recorder;
	std::thread m_ControlThread;
	std::atomic<bool> m_StopRequested{ false };

	// The cameras, which timed out and did not come back yet (guarded by m_ClientsMutex), to report them when they are back.
	std::set<std::string> m_OfflineCameras;
	std::thread m_ReaperThread;
	std::thread m_ForwardThread;
};

