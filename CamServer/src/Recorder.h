#pragma once

#include <Cam-Core.h>

#include <condition_variable>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// <summary>
/// One frame of a camera, as it was received (JPEG), and when. The ring buffer of every camera holds the frames of the last minutes.
/// </summary>
struct RecordedFrame
{
	std::shared_ptr<std::vector<unsigned char>> Data;

	/// <summary>
	/// The time, when the frame was received, in milliseconds since 1970 (wall clock).
	/// </summary>
	int64 TimeMS = 0;
};

/// <summary>
/// Writes JPEG frames into an AVI file (Motion JPEG). The frames are not decoded or encoded again, they are written as they are, so this is fast and
/// loses no quality. The files play in VLC, Windows Media Player and most other players. The file is only complete after Close().
/// </summary>
class AviMjpegWriter
{
public:

	AviMjpegWriter();
	~AviMjpegWriter();

	/// <summary>
	/// Creates the file. The size of the pictures is fixed for the whole file.
	/// </summary>
	bool Open(const std::string &path, uint32 width, uint32 height);

	/// <summary>
	/// Appends a frame. Returns false, if the file cannot be written (disk full).
	/// </summary>
	bool AddFrame(const unsigned char *jpeg, uint32 size);

	/// <summary>
	/// Writes the index and the final header. The frame rate is the average of the recording (frames per second), the frames have no fixed rate.
	/// </summary>
	bool Close(double fps);

	bool IsOpen() const { return m_File != nullptr; }
	uint32 GetFrameCount() const { return (uint32)m_Index.size(); }
	uint64 GetBytesWritten() const { return m_Bytes; }
	uint32 GetWidth() const { return m_Width; }
	uint32 GetHeight() const { return m_Height; }

	/// <summary>
	/// Reads the size of a JPEG picture from its header (without decoding it). Returns false, if it is not a valid JPEG.
	/// </summary>
	static bool GetJpegSize(const unsigned char *jpeg, uint32 size, uint32 *width, uint32 *height);

private:

	struct IndexEntry
	{
		uint32 Offset;
		uint32 Size;
	};

	void WriteU32(uint32 value);
	void WriteU16(uint16 value);
	void WriteFourCC(const char *fourcc);
	void PatchU32(long position, uint32 value);

	FILE *m_File = nullptr;
	uint32 m_Width = 0;
	uint32 m_Height = 0;
	uint64 m_Bytes = 0;
	uint32 m_MaxFrameSize = 0;
	bool m_Failed = false;
	std::vector<IndexEntry> m_Index;
};

/// <summary>
/// When to record: a list of time windows, for example "22:00-06:00" (every day, also over midnight) or "Mon-Fri 08:00-18:00; Sat,Sun 00:00-23:59".
/// </summary>
class RecordSchedule
{
public:

	/// <summary>
	/// Reads the schedule. Windows are separated by semicolons, every window is an optional list of days (Mon, Tue, Wed, Thu, Fri, Sat, Sun, ranges like
	/// Mon-Fri, or "daily"; without days it is every day) and a time range HH:MM-HH:MM in the local time. A range, which ends before it starts, goes over midnight.
	/// </summary>
	bool Parse(const std::string &text, std::string *error);

	/// <summary>
	/// True, if there is a window (an empty schedule never records).
	/// </summary>
	bool Empty() const { return m_Windows.empty(); }

	/// <summary>
	/// True, if the local time is inside of a window.
	/// </summary>
	bool IsActive(const std::tm &local) const;

private:

	struct Window
	{
		unsigned int Days = 0;		// bit 0 is Sunday, like tm_wday
		int StartMinute = 0;
		int EndMinute = 0;
	};

	std::vector<Window> m_Windows;
};

struct RecorderConfig
{
	/// <summary>
	/// The folder for all recordings, with one sub folder per camera.
	/// </summary>
	std::string Path = "recordings";

	/// <summary>
	/// The times, at which all frames are recorded (see RecordSchedule). Empty: no scheduled recording (saving the last minutes still works).
	/// </summary>
	std::string Schedule;

	/// <summary>
	/// The cameras, which are recorded on schedule. Empty: all cameras.
	/// </summary>
	std::vector<std::string> Cameras;

	/// <summary>
	/// A scheduled recording is split into files of this many minutes (a file is only playable, when it is complete).
	/// </summary>
	uint32 SegmentMinutes = 10;

	/// <summary>
	/// Recordings, which are older than this many days, are deleted. 0 keeps them forever.
	/// </summary>
	uint32 KeepDays = 14;

	/// <summary>
	/// If all recordings together are larger than this many gigabytes, the oldest ones are deleted. 0 means no limit.
	/// </summary>
	uint32 MaxGigabytes = 20;
};

/// <summary>
/// Saves frames to disk: the scheduled recording (continuously, on a thread of its own), and short clips on demand (the last minutes of a camera).
/// Also deletes old recordings.
/// </summary>
class Recorder
{
public:

	Recorder(const RecorderConfig &config);
	~Recorder();

	/// <summary>
	/// Reads the schedule and starts the recording thread. Logs the reason, if the schedule is invalid (the scheduled recording is off then).
	/// </summary>
	void Start();

	/// <summary>
	/// True, if a schedule is set.
	/// </summary>
	bool IsScheduled() const { return !m_Schedule.Empty(); }

	/// <summary>
	/// Hands a new frame of a camera to the recording. Fast, the frame is written by the recording thread (and only if the schedule says so).
	/// </summary>
	void AddFrame(uint32 cameraId, const std::string &camera, const RecordedFrame &frame);

	/// <summary>
	/// Writes frames of a camera into a new file (a clip).
	/// </summary>
	/// <param name="label">Part of the file name, for example "last5min".</param>
	/// <param name="outPath">Receives the path of the file.</param>
	/// <param name="error">Receives the reason, if it failed.</param>
	bool SaveClip(const std::string &camera, const std::vector<RecordedFrame> &frames, const std::string &label, std::string *outPath, std::string *error);

	/// <summary>
	/// The current time in milliseconds since 1970.
	/// </summary>
	static int64 NowMS();

private:

	struct Session
	{
		AviMjpegWriter Writer;
		std::string Camera;
		std::string Path;
		int64 FirstFrameMS = 0;
		int64 LastFrameMS = 0;
	};

	struct PendingFrame
	{
		uint32 CameraId;
		std::string Camera;
		RecordedFrame Frame;
	};

	void WriterLoop();
	void WriteScheduledFrame(const PendingFrame &pending);
	void CloseSession(uint32 cameraId);
	void CleanUpOldRecordings();
	std::string CameraFolder(const std::string &camera) const;

private:

	RecorderConfig m_Config;
	RecordSchedule m_Schedule;

	std::mutex m_Mutex;
	std::condition_variable m_Condition;
	std::deque<PendingFrame> m_Queue;
	std::thread m_Thread;
	bool m_Stop = false;
	int64 m_LastDropWarningMS = 0;

	// only used by the recording thread
	std::map<uint32, std::unique_ptr<Session>> m_Sessions;
	int64 m_LastCleanUpMS = 0;
};
