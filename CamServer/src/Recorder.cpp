#include "Recorder.h"

#ifdef _MSC_VER
#pragma warning(disable: 4996)
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Core/Log.h"

namespace
{
	// A file is closed and a new one is started, when it reaches this size. AVI files are limited to 4 GB (and some players have problems with more than 2 GB).
	const uint64 MAX_FILE_BYTES = 1500ull * 1024 * 1024;

	// More frames than this are not queued for the recording thread (the disk is too slow).
	const size_t MAX_QUEUED_FRAMES = 600;

	// A camera, which sends nothing for this long, ends its recording file (the file is finished, a new one is started, when it sends again).
	const int64 IDLE_CLOSE_MS = 60 * 1000;

	// Offsets of the values in the AVI header, which are filled in when the file is closed (the layout of the header is fixed).
	const long RIFF_SIZE_POSITION = 4;
	const long AVIH_MICROSECONDS_PER_FRAME = 32;
	const long AVIH_MAX_BYTES_PER_SECOND = 36;
	const long AVIH_TOTAL_FRAMES = 48;
	const long AVIH_SUGGESTED_BUFFER = 60;
	const long STRH_SCALE = 128;
	const long STRH_RATE = 132;
	const long STRH_LENGTH = 140;
	const long STRH_SUGGESTED_BUFFER = 144;
	const long MOVI_SIZE_POSITION = 216;
	const long MOVI_FOURCC_POSITION = 220;	// offsets in the index are counted from here
	const long FIRST_CHUNK_POSITION = 224;

	std::string SafeName(const std::string &name)
	{
		std::string result = name;
		for (char &c : result)
		{
			if (!isalnum((unsigned char)c) && c != '-' && c != '_')
			{
				c = '_';
			}
		}

		return result.empty() ? "unnamed" : result;
	}

	std::string LowerCase(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return text;
	}

	std::string Trim(const std::string &text)
	{
		size_t begin = text.find_first_not_of(" \t");
		if (begin == std::string::npos)
		{
			return "";
		}

		size_t end = text.find_last_not_of(" \t");
		return text.substr(begin, end - begin + 1);
	}

	std::tm LocalTime(int64 timeMS)
	{
		std::time_t seconds = (std::time_t)(timeMS / 1000);
		std::tm local = {};
#ifdef _WIN32
		localtime_s(&local, &seconds);
#else
		localtime_r(&seconds, &local);
#endif
		return local;
	}

	std::string TimestampName(int64 timeMS)
	{
		std::tm local = LocalTime(timeMS);
		char buffer[32];
		strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &local);
		return buffer;
	}

	double AverageFps(uint32 frames, int64 firstMS, int64 lastMS)
	{
		if (frames < 2 || lastMS <= firstMS)
		{
			return 10.0;
		}

		double fps = (double)(frames - 1) * 1000.0 / (double)(lastMS - firstMS);
		return std::min(std::max(fps, 0.1), 120.0);
	}
}

// ============================================================ AviMjpegWriter

AviMjpegWriter::AviMjpegWriter()
{
}

AviMjpegWriter::~AviMjpegWriter()
{
	if (m_File)
	{
		// A file, which was not closed properly, is still as good as possible (the frame rate is unknown, 10 is assumed).
		Close(10.0);
	}
}

void AviMjpegWriter::WriteU32(uint32 value)
{
	unsigned char bytes[4] = { (unsigned char)(value & 255), (unsigned char)((value >> 8) & 255), (unsigned char)((value >> 16) & 255), (unsigned char)((value >> 24) & 255) };
	if (fwrite(bytes, 1, 4, m_File) != 4)
	{
		m_Failed = true;
	}

	m_Bytes += 4;
}

void AviMjpegWriter::WriteU16(uint16 value)
{
	unsigned char bytes[2] = { (unsigned char)(value & 255), (unsigned char)((value >> 8) & 255) };
	if (fwrite(bytes, 1, 2, m_File) != 2)
	{
		m_Failed = true;
	}

	m_Bytes += 2;
}

void AviMjpegWriter::WriteFourCC(const char *fourcc)
{
	if (fwrite(fourcc, 1, 4, m_File) != 4)
	{
		m_Failed = true;
	}

	m_Bytes += 4;
}

void AviMjpegWriter::PatchU32(long position, uint32 value)
{
	unsigned char bytes[4] = { (unsigned char)(value & 255), (unsigned char)((value >> 8) & 255), (unsigned char)((value >> 16) & 255), (unsigned char)((value >> 24) & 255) };
	if (fseek(m_File, position, SEEK_SET) != 0 || fwrite(bytes, 1, 4, m_File) != 4)
	{
		m_Failed = true;
	}
}

bool AviMjpegWriter::Open(const std::string &path, uint32 width, uint32 height)
{
	if (m_File)
	{
		return false;
	}

	m_File = fopen(path.c_str(), "wb");
	if (!m_File)
	{
		return false;
	}

	m_Width = width;
	m_Height = height;
	m_Bytes = 0;
	m_Failed = false;
	m_MaxFrameSize = 0;
	m_Index.clear();

	// The header has a fixed size, so the values, which are only known at the end (frame count, frame rate, sizes), can be filled in later.
	WriteFourCC("RIFF");
	WriteU32(0);						// size of the file, filled in at the end
	WriteFourCC("AVI ");

	WriteFourCC("LIST");
	WriteU32(192);						// size of this list
	WriteFourCC("hdrl");

	WriteFourCC("avih");
	WriteU32(56);
	WriteU32(100000);					// microseconds per frame (at the end)
	WriteU32(0);						// maximum bytes per second (at the end)
	WriteU32(0);						// padding
	WriteU32(0x10);						// flags: has an index
	WriteU32(0);						// number of frames (at the end)
	WriteU32(0);						// initial frames
	WriteU32(1);						// number of streams
	WriteU32(0);						// suggested buffer size (at the end)
	WriteU32(width);
	WriteU32(height);
	WriteU32(0); WriteU32(0); WriteU32(0); WriteU32(0);	// reserved

	WriteFourCC("LIST");
	WriteU32(116);
	WriteFourCC("strl");

	WriteFourCC("strh");
	WriteU32(56);
	WriteFourCC("vids");
	WriteFourCC("MJPG");
	WriteU32(0);						// flags
	WriteU16(0);						// priority
	WriteU16(0);						// language
	WriteU32(0);						// initial frames
	WriteU32(1000);						// scale (at the end)
	WriteU32(10000);					// rate (at the end): frames per second = rate / scale
	WriteU32(0);						// start
	WriteU32(0);						// length in frames (at the end)
	WriteU32(0);						// suggested buffer size (at the end)
	WriteU32(0xFFFFFFFF);				// quality
	WriteU32(0);						// sample size
	WriteU16(0); WriteU16(0); WriteU16((uint16)width); WriteU16((uint16)height);	// frame rectangle

	WriteFourCC("strf");
	WriteU32(40);
	WriteU32(40);						// size of the bitmap header
	WriteU32(width);
	WriteU32(height);
	WriteU16(1);						// planes
	WriteU16(24);						// bits per pixel
	WriteFourCC("MJPG");
	WriteU32(width * height * 3);
	WriteU32(0); WriteU32(0); WriteU32(0); WriteU32(0);

	WriteFourCC("LIST");
	WriteU32(0);						// size of the movie data (at the end)
	WriteFourCC("movi");

	if (m_Failed || m_Bytes != (uint64)FIRST_CHUNK_POSITION)
	{
		fclose(m_File);
		m_File = nullptr;
		return false;
	}

	return true;
}

bool AviMjpegWriter::AddFrame(const unsigned char *jpeg, uint32 size)
{
	if (!m_File || m_Failed || size == 0)
	{
		return false;
	}

	// The index and the end of the file need room too.
	uint64 needed = 8 + (uint64)size + 1 + 16 * (uint64)(m_Index.size() + 1) + 64;
	if (m_Bytes + needed > MAX_FILE_BYTES + (MAX_FILE_BYTES / 4))
	{
		return false;
	}

	IndexEntry entry;
	entry.Offset = (uint32)(m_Bytes - (uint64)MOVI_FOURCC_POSITION);
	entry.Size = size;

	WriteFourCC("00dc");
	WriteU32(size);
	if (fwrite(jpeg, 1, size, m_File) != size)
	{
		m_Failed = true;
		return false;
	}

	m_Bytes += size;

	// Chunks start at even positions.
	if (size & 1)
	{
		unsigned char padding = 0;
		if (fwrite(&padding, 1, 1, m_File) != 1)
		{
			m_Failed = true;
			return false;
		}

		m_Bytes += 1;
	}

	m_Index.push_back(entry);
	m_MaxFrameSize = std::max(m_MaxFrameSize, size);
	return !m_Failed;
}

bool AviMjpegWriter::Close(double fps)
{
	if (!m_File)
	{
		return false;
	}

	fps = std::min(std::max(fps, 0.1), 120.0);
	uint64 movie_end = m_Bytes;

	// the index of all frames
	WriteFourCC("idx1");
	WriteU32((uint32)(16 * m_Index.size()));
	for (const IndexEntry &entry : m_Index)
	{
		WriteFourCC("00dc");
		WriteU32(0x10);					// key frame (every JPEG is one)
		WriteU32(entry.Offset);
		WriteU32(entry.Size);
	}

	uint64 file_size = m_Bytes;

	// everything, which was not known when the file was started
	PatchU32(RIFF_SIZE_POSITION, (uint32)(file_size - 8));
	PatchU32(MOVI_SIZE_POSITION, (uint32)(movie_end - (uint64)MOVI_FOURCC_POSITION));
	PatchU32(AVIH_MICROSECONDS_PER_FRAME, (uint32)(1000000.0 / fps));
	PatchU32(AVIH_MAX_BYTES_PER_SECOND, (uint32)((double)m_MaxFrameSize * fps));
	PatchU32(AVIH_TOTAL_FRAMES, (uint32)m_Index.size());
	PatchU32(AVIH_SUGGESTED_BUFFER, m_MaxFrameSize);
	PatchU32(STRH_SCALE, 1000);
	PatchU32(STRH_RATE, (uint32)(fps * 1000.0));
	PatchU32(STRH_LENGTH, (uint32)m_Index.size());
	PatchU32(STRH_SUGGESTED_BUFFER, m_MaxFrameSize);

	bool success = !m_Failed && fclose(m_File) == 0;
	m_File = nullptr;
	return success;
}

bool AviMjpegWriter::GetJpegSize(const unsigned char *jpeg, uint32 size, uint32 *width, uint32 *height)
{
	if (size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
	{
		return false;
	}

	uint32 i = 2;
	while (i + 4 < size)
	{
		if (jpeg[i] != 0xFF)
		{
			return false;
		}

		unsigned char marker = jpeg[i + 1];
		if (marker == 0xFF)
		{
			++i;	// fill byte
			continue;
		}

		// Start of frame (all kinds except the tables and the arithmetic coding markers in between): the size is stored here.
		if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
		{
			if (i + 9 > size)
			{
				return false;
			}

			*height = ((uint32)jpeg[i + 5] << 8) | jpeg[i + 6];
			*width = ((uint32)jpeg[i + 7] << 8) | jpeg[i + 8];
			return *width > 0 && *height > 0;
		}

		if (marker == 0xD9 || marker == 0xDA)
		{
			return false;
		}

		uint32 length = ((uint32)jpeg[i + 2] << 8) | jpeg[i + 3];
		i += 2 + length;
	}

	return false;
}

// ============================================================ RecordSchedule

bool RecordSchedule::Parse(const std::string &text, std::string *error)
{
	m_Windows.clear();

	static const char *day_names[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
	auto day_index = [&](const std::string &name) -> int
	{
		std::string lower = LowerCase(Trim(name));
		if (lower.size() < 3)
		{
			return -1;
		}

		for (int i = 0; i < 7; ++i)
		{
			if (lower.compare(0, 3, day_names[i]) == 0)
			{
				return i;
			}
		}

		return -1;
	};

	size_t start = 0;
	while (start <= text.size())
	{
		size_t end = text.find(';', start);
		std::string part = Trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
		start = end == std::string::npos ? text.size() + 1 : end + 1;
		if (part.empty())
		{
			continue;
		}

		// "Mon-Fri 08:00-18:00": the time is the last word, the days (if any) come before it.
		size_t space = part.find_last_of(" \t");
		std::string time_text = space == std::string::npos ? part : Trim(part.substr(space + 1));
		std::string days_text = space == std::string::npos ? "" : Trim(part.substr(0, space));

		int start_hour, start_minute, end_hour, end_minute;
		if (sscanf(time_text.c_str(), "%d:%d-%d:%d", &start_hour, &start_minute, &end_hour, &end_minute) != 4 ||
			start_hour < 0 || start_hour > 23 || end_hour < 0 || end_hour > 24 || start_minute < 0 || start_minute > 59 || end_minute < 0 || end_minute > 59)
		{
			*error = "'" + part + "' is not a time range like 22:00-06:00";
			m_Windows.clear();
			return false;
		}

		Window window;
		window.StartMinute = start_hour * 60 + start_minute;
		window.EndMinute = end_hour * 60 + end_minute;

		std::string lower_days = LowerCase(days_text);
		if (lower_days.empty() || lower_days == "daily" || lower_days == "all" || lower_days == "every day")
		{
			window.Days = 0x7F;
		}
		else
		{
			// Mon,Wed,Fri or Mon-Fri or a mixture
			size_t day_start = 0;
			while (day_start <= days_text.size())
			{
				size_t day_end = days_text.find(',', day_start);
				std::string item = Trim(days_text.substr(day_start, day_end == std::string::npos ? std::string::npos : day_end - day_start));
				day_start = day_end == std::string::npos ? days_text.size() + 1 : day_end + 1;
				if (item.empty())
				{
					continue;
				}

				size_t dash = item.find('-');
				int first = day_index(dash == std::string::npos ? item : item.substr(0, dash));
				int last = dash == std::string::npos ? first : day_index(item.substr(dash + 1));
				if (first < 0 || last < 0)
				{
					*error = "'" + item + "' is not a day (Mon, Tue, Wed, Thu, Fri, Sat, Sun, or a range like Mon-Fri)";
					m_Windows.clear();
					return false;
				}

				// Fri-Mon goes over the end of the week.
				for (int day = first;; day = (day + 1) % 7)
				{
					window.Days |= 1u << day;
					if (day == last)
					{
						break;
					}
				}
			}
		}

		m_Windows.push_back(window);
	}

	return true;
}

bool RecordSchedule::IsActive(const std::tm &local) const
{
	int minute = local.tm_hour * 60 + local.tm_min;
	int day = local.tm_wday;
	int previous_day = (day + 6) % 7;

	for (const Window &window : m_Windows)
	{
		bool today = (window.Days & (1u << day)) != 0;
		bool yesterday = (window.Days & (1u << previous_day)) != 0;

		if (window.StartMinute == window.EndMinute)
		{
			// the same time twice: the whole day
			if (today)
			{
				return true;
			}
		}
		else if (window.StartMinute < window.EndMinute)
		{
			if (today && minute >= window.StartMinute && minute < window.EndMinute)
			{
				return true;
			}
		}
		else
		{
			// over midnight: the evening belongs to the day, which started the window, the morning to the next day
			if ((today && minute >= window.StartMinute) || (yesterday && minute < window.EndMinute))
			{
				return true;
			}
		}
	}

	return false;
}

// ============================================================ Recorder

Recorder::Recorder(const RecorderConfig &config)
	: m_Config(config)
{
}

Recorder::~Recorder()
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Stop = true;
	}

	m_Condition.notify_all();
	if (m_Thread.joinable())
	{
		m_Thread.join();
	}
}

int64 Recorder::NowMS()
{
	return (int64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Recorder::Start()
{
	if (!m_Config.Schedule.empty())
	{
		std::string error;
		if (!m_Schedule.Parse(m_Config.Schedule, &error))
		{
			CAM_LOG_ERROR("The recording schedule is invalid: {}. Nothing is recorded on schedule.", error);
		}
		else
		{
			CAM_LOG_INFO("Recording on schedule: {0}, {1}, files of {2} minutes in {3}", m_Config.Schedule,
				m_Config.Cameras.empty() ? std::string("all cameras") : std::to_string(m_Config.Cameras.size()) + " camera(s)", m_Config.SegmentMinutes, m_Config.Path);
		}
	}

	m_Thread = std::thread(&Recorder::WriterLoop, this);
}

std::string Recorder::CameraFolder(const std::string &camera) const
{
	return (std::filesystem::path(m_Config.Path) / SafeName(camera)).string();
}

void Recorder::AddFrame(uint32 cameraId, const std::string &camera, const RecordedFrame &frame)
{
	if (m_Schedule.Empty() || !frame.Data)
	{
		return;
	}

	if (!m_Config.Cameras.empty())
	{
		bool wanted = false;
		for (const std::string &name : m_Config.Cameras)
		{
			if (LowerCase(name) == LowerCase(camera))
			{
				wanted = true;
				break;
			}
		}

		if (!wanted)
		{
			return;
		}
	}

	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Queue.size() >= MAX_QUEUED_FRAMES)
		{
			// The disk is too slow. Dropping a frame is better than using up all memory.
			int64 now = NowMS();
			if (now - m_LastDropWarningMS > 10000)
			{
				m_LastDropWarningMS = now;
				CAM_LOG_WARN("The recording cannot keep up with the cameras (the disk is too slow), frames are dropped.");
			}

			return;
		}

		m_Queue.push_back({ cameraId, camera, frame });
	}

	m_Condition.notify_one();
}

void Recorder::WriterLoop()
{
	for (;;)
	{
		std::vector<PendingFrame> frames;
		bool stop = false;
		{
			std::unique_lock<std::mutex> lock(m_Mutex);
			m_Condition.wait_for(lock, std::chrono::seconds(1), [this] { return m_Stop || !m_Queue.empty(); });
			stop = m_Stop;
			while (!m_Queue.empty())
			{
				frames.push_back(std::move(m_Queue.front()));
				m_Queue.pop_front();
			}
		}

		for (const PendingFrame &pending : frames)
		{
			WriteScheduledFrame(pending);
		}

		// A camera, which went silent, finishes its file.
		int64 now = NowMS();
		std::vector<uint32> idle;
		for (const auto &entry : m_Sessions)
		{
			if (now - entry.second->LastFrameMS > IDLE_CLOSE_MS)
			{
				idle.push_back(entry.first);
			}
		}

		for (uint32 id : idle)
		{
			CloseSession(id);
		}

		// Old recordings are deleted from time to time.
		if (now - m_LastCleanUpMS > 10 * 60 * 1000)
		{
			m_LastCleanUpMS = now;
			CleanUpOldRecordings();
		}

		if (stop)
		{
			break;
		}
	}

	// A finished file, not a damaged one.
	while (!m_Sessions.empty())
	{
		CloseSession(m_Sessions.begin()->first);
	}
}

void Recorder::WriteScheduledFrame(const PendingFrame &pending)
{
	const unsigned char *jpeg = pending.Frame.Data->data();
	uint32 size = (uint32)pending.Frame.Data->size();

	bool active = m_Schedule.IsActive(LocalTime(pending.Frame.TimeMS));
	auto found = m_Sessions.find(pending.CameraId);
	if (!active)
	{
		// The window is over.
		if (found != m_Sessions.end())
		{
			CloseSession(pending.CameraId);
		}

		return;
	}

	// A file, which is long enough, is finished, and the next frame starts a new one.
	if (found != m_Sessions.end())
	{
		Session &session = *found->second;
		bool segment_over = pending.Frame.TimeMS - session.FirstFrameMS >= (int64)m_Config.SegmentMinutes * 60 * 1000;
		if (segment_over || session.Writer.GetBytesWritten() >= MAX_FILE_BYTES)
		{
			CloseSession(pending.CameraId);
			found = m_Sessions.end();
		}
	}

	if (found == m_Sessions.end())
	{
		uint32 width = 0, height = 0;
		if (!AviMjpegWriter::GetJpegSize(jpeg, size, &width, &height))
		{
			return;
		}

		std::error_code error;
		std::filesystem::create_directories(CameraFolder(pending.Camera), error);

		auto session = std::make_unique<Session>();
		session->Camera = pending.Camera;
		session->Path = (std::filesystem::path(CameraFolder(pending.Camera)) / (TimestampName(pending.Frame.TimeMS) + "_scheduled.avi")).string();
		if (!session->Writer.Open(session->Path, width, height))
		{
			CAM_LOG_ERROR("Could not create the recording file {}", session->Path);
			return;
		}

		session->FirstFrameMS = pending.Frame.TimeMS;
		CAM_LOG_INFO("Recording of {0} started: {1}", pending.Camera, session->Path);
		found = m_Sessions.emplace(pending.CameraId, std::move(session)).first;
	}

	Session &session = *found->second;
	uint32 width = 0, height = 0;
	if (!AviMjpegWriter::GetJpegSize(jpeg, size, &width, &height) || width != session.Writer.GetWidth() || height != session.Writer.GetHeight())
	{
		// The size of a video cannot change, a different size starts a new file.
		CloseSession(pending.CameraId);
		return;
	}

	if (!session.Writer.AddFrame(jpeg, size))
	{
		CAM_LOG_ERROR("Could not write the recording {} (disk full?), the file is closed.", session.Path);
		CloseSession(pending.CameraId);
		return;
	}

	session.LastFrameMS = pending.Frame.TimeMS;
}

void Recorder::CloseSession(uint32 cameraId)
{
	auto found = m_Sessions.find(cameraId);
	if (found == m_Sessions.end())
	{
		return;
	}

	Session &session = *found->second;
	uint32 frames = session.Writer.GetFrameCount();
	double fps = AverageFps(frames, session.FirstFrameMS, session.LastFrameMS);
	std::string path = session.Path;
	std::string camera = session.Camera;
	uint64 bytes = session.Writer.GetBytesWritten();
	bool success = session.Writer.Close(fps);
	m_Sessions.erase(found);

	if (success)
	{
		CAM_LOG_INFO("Recording of {0} saved: {1} ({2} frames, {3:.1f} MB)", camera, path, frames, bytes / 1048576.0);
	}
	else
	{
		CAM_LOG_ERROR("The recording {} could not be finished.", path);
	}
}

bool Recorder::SaveClip(const std::string &camera, const std::vector<RecordedFrame> &frames, const std::string &label, std::string *outPath, std::string *error)
{
	if (frames.empty())
	{
		*error = "there are no frames of the camera " + camera;
		return false;
	}

	uint32 width = 0, height = 0;
	if (!AviMjpegWriter::GetJpegSize(frames.front().Data->data(), (uint32)frames.front().Data->size(), &width, &height))
	{
		*error = "the frames of the camera " + camera + " are not valid JPEG pictures";
		return false;
	}

	std::error_code folder_error;
	std::filesystem::create_directories(CameraFolder(camera), folder_error);

	// The clip is named after its end (the moment, which was asked for, for example the unknown person). Saving twice never overwrites a file.
	std::string base = TimestampName(frames.back().TimeMS) + "_" + SafeName(label);
	std::string path = (std::filesystem::path(CameraFolder(camera)) / (base + ".avi")).string();
	for (int number = 2; std::filesystem::exists(path, folder_error) && number < 1000; ++number)
	{
		path = (std::filesystem::path(CameraFolder(camera)) / (base + "_" + std::to_string(number) + ".avi")).string();
	}

	AviMjpegWriter writer;
	if (!writer.Open(path, width, height))
	{
		*error = "could not create the file " + path;
		return false;
	}

	int64 first_ms = 0, last_ms = 0;
	for (const RecordedFrame &frame : frames)
	{
		uint32 frame_width = 0, frame_height = 0;
		if (!frame.Data || !AviMjpegWriter::GetJpegSize(frame.Data->data(), (uint32)frame.Data->size(), &frame_width, &frame_height) || frame_width != width || frame_height != height)
		{
			continue;
		}

		if (!writer.AddFrame(frame.Data->data(), (uint32)frame.Data->size()))
		{
			break;
		}

		if (writer.GetFrameCount() == 1)
		{
			first_ms = frame.TimeMS;
		}

		last_ms = frame.TimeMS;
	}

	uint32 written = writer.GetFrameCount();
	if (!writer.Close(AverageFps(written, first_ms, last_ms)) || written == 0)
	{
		*error = "could not write the file " + path;
		return false;
	}

	*outPath = path;
	return true;
}

void Recorder::CleanUpOldRecordings()
{
	namespace fs = std::filesystem;
	if (m_Config.KeepDays == 0 && m_Config.MaxGigabytes == 0)
	{
		return;
	}

	std::error_code error;
	if (!fs::exists(m_Config.Path, error))
	{
		return;
	}

	struct Entry
	{
		fs::path Path;
		fs::file_time_type Time;
		uint64 Size;
	};

	std::vector<Entry> recordings;
	uint64 total = 0;
	for (fs::recursive_directory_iterator it(m_Config.Path, error), end; !error && it != end; it.increment(error))
	{
		std::error_code entry_error;
		if (!it->is_regular_file(entry_error) || it->path().extension() != ".avi")
		{
			continue;
		}

		// A file, which is being written, stays.
		bool in_use = false;
		for (const auto &session : m_Sessions)
		{
			if (fs::path(session.second->Path) == it->path())
			{
				in_use = true;
			}
		}

		uint64 size = (uint64)it->file_size(entry_error);
		auto time = it->last_write_time(entry_error);
		if (entry_error)
		{
			continue;
		}

		total += size;
		if (!in_use)
		{
			recordings.push_back({ it->path(), time, size });
		}
	}

	std::sort(recordings.begin(), recordings.end(), [](const Entry &a, const Entry &b) { return a.Time < b.Time; });

	auto remove = [&](const Entry &entry, const char *reason)
	{
		std::error_code remove_error;
		if (fs::remove(entry.Path, remove_error))
		{
			CAM_LOG_INFO("Deleted the old recording {0} ({1})", entry.Path.string(), reason);
			total -= std::min(total, entry.Size);
			return true;
		}

		return false;
	};

	size_t next = 0;
	if (m_Config.KeepDays > 0)
	{
		auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * m_Config.KeepDays);
		for (; next < recordings.size() && recordings[next].Time < limit; ++next)
		{
			remove(recordings[next], "older than record_keep_days");
		}
	}

	if (m_Config.MaxGigabytes > 0)
	{
		uint64 max_bytes = (uint64)m_Config.MaxGigabytes * 1024ull * 1024ull * 1024ull;
		for (; next < recordings.size() && total > max_bytes; ++next)
		{
			remove(recordings[next], "record_max_gb is exceeded");
		}
	}
}
