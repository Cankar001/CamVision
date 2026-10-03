#include "CamTest.h"
#include "TestUtils.h"

#include "Recorder.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

// ---------------------------------------------------------------------------------------------------------- RecordSchedule

// 2024-01-01 was a Monday.
static std::tm Time(int day_of_week, int hour, int minute)
{
	std::tm time = {};
	time.tm_year = 124;
	time.tm_mon = 0;
	time.tm_mday = 1;
	time.tm_wday = day_of_week;
	time.tm_hour = hour;
	time.tm_min = minute;
	return time;
}

static constexpr int SUN = 0, MON = 1, TUE = 2, WED = 3, THU = 4, FRI = 5, SAT = 6;

TEST(RecordSchedule, EmptyScheduleNeverRecords)
{
	RecordSchedule schedule;
	std::string error;
	REQUIRE(schedule.Parse("", &error));
	CHECK(schedule.Empty());
	CHECK(!schedule.IsActive(Time(MON, 12, 0)));
}

TEST(RecordSchedule, SimpleWindowEveryDay)
{
	RecordSchedule schedule;
	std::string error;
	REQUIRE(schedule.Parse("08:00-18:00", &error));
	CHECK(!schedule.Empty());

	for (int day = SUN; day <= SAT; ++day)
	{
		CHECK(schedule.IsActive(Time(day, 8, 0)));
		CHECK(schedule.IsActive(Time(day, 12, 30)));
		CHECK(schedule.IsActive(Time(day, 17, 59)));
		CHECK(!schedule.IsActive(Time(day, 18, 0)));		// the end is not part of the window
		CHECK(!schedule.IsActive(Time(day, 7, 59)));
	}
}

TEST(RecordSchedule, WindowOverMidnightBelongsToTheDayItStarts)
{
	RecordSchedule schedule;
	std::string error;
	REQUIRE(schedule.Parse("Fri 22:00-06:00", &error));

	CHECK(!schedule.IsActive(Time(FRI, 21, 59)));
	CHECK(schedule.IsActive(Time(FRI, 22, 0)));
	CHECK(schedule.IsActive(Time(FRI, 23, 59)));
	CHECK(schedule.IsActive(Time(SAT, 0, 0)));
	CHECK(schedule.IsActive(Time(SAT, 5, 59)));
	CHECK(!schedule.IsActive(Time(SAT, 6, 0)));
	CHECK(!schedule.IsActive(Time(SAT, 23, 0)));		// Saturday evening was not asked for
	CHECK(!schedule.IsActive(Time(FRI, 3, 0)));			// and neither was Friday morning
}

TEST(RecordSchedule, DayRangesAndLists)
{
	RecordSchedule schedule;
	std::string error;
	REQUIRE(schedule.Parse("Mon-Fri 08:00-18:00", &error));
	CHECK(schedule.IsActive(Time(MON, 9, 0)));
	CHECK(schedule.IsActive(Time(FRI, 9, 0)));
	CHECK(!schedule.IsActive(Time(SAT, 9, 0)));
	CHECK(!schedule.IsActive(Time(SUN, 9, 0)));

	REQUIRE(schedule.Parse("Mon,Wed,Fri 10:00-11:00", &error));
	CHECK(schedule.IsActive(Time(MON, 10, 30)));
	CHECK(!schedule.IsActive(Time(TUE, 10, 30)));
	CHECK(schedule.IsActive(Time(WED, 10, 30)));
	CHECK(!schedule.IsActive(Time(THU, 10, 30)));
	CHECK(schedule.IsActive(Time(FRI, 10, 30)));
}

TEST(RecordSchedule, RangeOverTheEndOfTheWeek)
{
	RecordSchedule schedule;
	std::string error;
	REQUIRE(schedule.Parse("Fri-Mon 10:00-11:00", &error));
	CHECK(schedule.IsActive(Time(FRI, 10, 30)));
	CHECK(schedule.IsActive(Time(SAT, 10, 30)));
	CHECK(schedule.IsActive(Time(SUN, 10, 30)));
	CHECK(schedule.IsActive(Time(MON, 10, 30)));
	CHECK(!schedule.IsActive(Time(TUE, 10, 30)));
}

TEST(RecordSchedule, SeveralWindows)
{
	RecordSchedule schedule;
	std::string error;
	REQUIRE(schedule.Parse("Mon-Fri 08:00-12:00; Sat,Sun 00:00-23:59", &error));
	CHECK(schedule.IsActive(Time(TUE, 9, 0)));
	CHECK(!schedule.IsActive(Time(TUE, 13, 0)));
	CHECK(schedule.IsActive(Time(SAT, 13, 0)));
	CHECK(schedule.IsActive(Time(SUN, 0, 0)));
}

TEST(RecordSchedule, DayNamesAreCaseInsensitive)
{
	RecordSchedule schedule;
	std::string error;
	REQUIRE(schedule.Parse("monday 08:00-09:00; WED 08:00-09:00", &error));
	CHECK(schedule.IsActive(Time(MON, 8, 30)));
	CHECK(schedule.IsActive(Time(WED, 8, 30)));
}

TEST(RecordSchedule, InvalidSchedulesAreRefusedWithAnError)
{
	RecordSchedule schedule;
	for (const char *text : { "banana", "25:00-26:00", "08:00", "Foo 08:00-09:00", "08:00-09:61", "Mon- 08:00-09:00" })
	{
		std::string error;
		CHECK(!schedule.Parse(text, &error));
		CHECK(!error.empty());
	}
}

// ---------------------------------------------------------------------------------------------------------- AviMjpegWriter

// A JPEG, which is not a real picture, but has everything, which is looked at without decoding it: the markers and the size.
static std::vector<unsigned char> FakeJpeg(uint32 width, uint32 height, size_t payload = 50, unsigned char fill = 0x77)
{
	std::vector<unsigned char> jpeg = { 0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08,
		(unsigned char)(height >> 8), (unsigned char)height, (unsigned char)(width >> 8), (unsigned char)width,
		0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01 };
	jpeg.insert(jpeg.end(), payload, fill);
	jpeg.push_back(0xFF);
	jpeg.push_back(0xD9);
	return jpeg;
}

static std::vector<unsigned char> ReadAll(const std::string &path)
{
	std::ifstream stream(path, std::ios::binary);
	return std::vector<unsigned char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

static uint32 U32(const std::vector<unsigned char> &data, size_t offset)
{
	return (uint32)data[offset] | ((uint32)data[offset + 1] << 8) | ((uint32)data[offset + 2] << 16) | ((uint32)data[offset + 3] << 24);
}

static size_t CountOf(const std::vector<unsigned char> &data, const char *four_cc)
{
	size_t count = 0;
	for (size_t i = 0; i + 4 <= data.size(); ++i)
	{
		if (memcmp(data.data() + i, four_cc, 4) == 0)
		{
			++count;
		}
	}

	return count;
}

TEST(AviMjpegWriter, ReadsTheSizeOfAJpeg)
{
	std::vector<unsigned char> jpeg = FakeJpeg(1920, 1080);
	uint32 width = 0, height = 0;
	REQUIRE(AviMjpegWriter::GetJpegSize(jpeg.data(), (uint32)jpeg.size(), &width, &height));
	CHECK_EQ(width, 1920u);
	CHECK_EQ(height, 1080u);
}

TEST(AviMjpegWriter, RefusesDataThatIsNoJpeg)
{
	uint32 width = 0, height = 0;
	unsigned char text[] = "this is not a jpeg picture";
	CHECK(!AviMjpegWriter::GetJpegSize(text, (uint32)sizeof(text), &width, &height));
	CHECK(!AviMjpegWriter::GetJpegSize(text, 0, &width, &height));

	// Cut off before the size.
	std::vector<unsigned char> jpeg = FakeJpeg(10, 10);
	CHECK(!AviMjpegWriter::GetJpegSize(jpeg.data(), 8, &width, &height));
}

TEST(AviMjpegWriter, WritesAWellFormedFile)
{
	TempDir dir;
	std::string path = dir.File("video.avi");

	AviMjpegWriter writer;
	REQUIRE(writer.Open(path, 640, 480));
	CHECK(writer.IsOpen());

	const uint32 frame_count = 25;
	size_t jpeg_bytes = 0;
	for (uint32 i = 0; i < frame_count; ++i)
	{
		// Frames of odd and even sizes (the chunks in an AVI are padded to even sizes).
		std::vector<unsigned char> jpeg = FakeJpeg(640, 480, 100 + i, (unsigned char)i);
		jpeg_bytes += jpeg.size();
		REQUIRE(writer.AddFrame(jpeg.data(), (uint32)jpeg.size()));
	}

	CHECK_EQ(writer.GetFrameCount(), frame_count);
	REQUIRE(writer.Close(10.0));
	CHECK(!writer.IsOpen());

	std::vector<unsigned char> file = ReadAll(path);
	REQUIRE(file.size() > 12 + jpeg_bytes);

	// RIFF container, which has the right size in its header.
	CHECK(memcmp(file.data(), "RIFF", 4) == 0);
	CHECK(memcmp(file.data() + 8, "AVI ", 4) == 0);
	CHECK_EQ((size_t)U32(file, 4) + 8, file.size());

	// One chunk per frame, and an index entry for every one of them.
	CHECK_EQ(CountOf(file, "avih"), (size_t)1);
	CHECK_EQ(CountOf(file, "idx1"), (size_t)1);
	CHECK(CountOf(file, "00dc") >= frame_count);
	CHECK(CountOf(file, "MJPG") >= 1);
}

TEST(AviMjpegWriter, AnAviFileHasTheNumberOfFramesInItsHeader)
{
	TempDir dir;
	std::string path = dir.File("video.avi");

	AviMjpegWriter writer;
	REQUIRE(writer.Open(path, 320, 240));
	for (int i = 0; i < 7; ++i)
	{
		std::vector<unsigned char> jpeg = FakeJpeg(320, 240);
		REQUIRE(writer.AddFrame(jpeg.data(), (uint32)jpeg.size()));
	}

	REQUIRE(writer.Close(5.0));

	std::vector<unsigned char> file = ReadAll(path);
	REQUIRE(file.size() > 80);

	// RIFF(12) + LIST hdrl(12) + "avih" + size(8): the main header starts at 32. Fields: microseconds per frame, bytes per second, padding, flags,
	// total frames, initial frames, streams, buffer size, width, height.
	CHECK_EQ(U32(file, 32), 200000u);		// 5 frames per second
	CHECK_EQ(U32(file, 48), 7u);
	CHECK_EQ(U32(file, 64), 320u);
	CHECK_EQ(U32(file, 68), 240u);
}

TEST(AviMjpegWriter, CannotOpenAFileInAMissingFolder)
{
	TempDir dir;
	AviMjpegWriter writer;
	CHECK(!writer.Open(dir.File("missing_folder/video.avi"), 640, 480));
	CHECK(!writer.IsOpen());
}

// ---------------------------------------------------------------------------------------------------------- Recorder

static RecordedFrame MakeFrame(uint32 width, uint32 height, int64 time_ms)
{
	RecordedFrame frame;
	frame.Data = std::make_shared<std::vector<unsigned char>>(FakeJpeg(width, height));
	frame.TimeMS = time_ms;
	return frame;
}

TEST(Recorder, SavesAClipOfTheBufferedFrames)
{
	TempDir dir;
	RecorderConfig config;
	config.Path = dir.File("recordings");
	Recorder recorder(config);

	std::vector<RecordedFrame> frames;
	for (int i = 0; i < 30; ++i)
	{
		frames.push_back(MakeFrame(640, 480, 1700000000000 + i * 100));
	}

	std::string path, error;
	REQUIRE(recorder.SaveClip("Front Door", frames, "manual", &path, &error));
	CHECK(error.empty());

	// Stored in a folder of the camera (the name is made safe for the file system), and named after the end of the clip and the reason.
	REQUIRE(Core::FileSystem::Get()->FileExists(path));
	CHECK(std::filesystem::path(path).parent_path().filename().string() == "Front_Door");
	CHECK(path.find("_manual.avi") != std::string::npos);

	std::vector<unsigned char> file = ReadAll(path);
	CHECK(memcmp(file.data(), "RIFF", 4) == 0);
	CHECK_EQ(U32(file, 48), 30u);
}

TEST(Recorder, SavingTwiceNeverOverwrites)
{
	TempDir dir;
	RecorderConfig config;
	config.Path = dir.File("recordings");
	Recorder recorder(config);

	std::vector<RecordedFrame> frames = { MakeFrame(320, 240, 1700000000000), MakeFrame(320, 240, 1700000000100) };

	std::string first, second, error;
	REQUIRE(recorder.SaveClip("cam", frames, "manual", &first, &error));
	REQUIRE(recorder.SaveClip("cam", frames, "manual", &second, &error));
	CHECK(first != second);
	CHECK(Core::FileSystem::Get()->FileExists(first));
	CHECK(Core::FileSystem::Get()->FileExists(second));
}

TEST(Recorder, NothingToSaveIsAnError)
{
	TempDir dir;
	RecorderConfig config;
	config.Path = dir.File("recordings");
	Recorder recorder(config);

	std::string path, error;
	CHECK(!recorder.SaveClip("cam", {}, "manual", &path, &error));
	CHECK(!error.empty());

	std::vector<RecordedFrame> broken(1);
	broken[0].Data = std::make_shared<std::vector<unsigned char>>(10, 0);
	error.clear();
	CHECK(!recorder.SaveClip("cam", broken, "manual", &path, &error));
	CHECK(!error.empty());
}

TEST(Recorder, FramesWithAnotherSizeAreLeftOut)
{
	// The size of a video cannot change.
	TempDir dir;
	RecorderConfig config;
	config.Path = dir.File("recordings");
	Recorder recorder(config);

	std::vector<RecordedFrame> frames = { MakeFrame(640, 480, 1700000000000), MakeFrame(320, 240, 1700000000100), MakeFrame(640, 480, 1700000000200) };

	std::string path, error;
	REQUIRE(recorder.SaveClip("cam", frames, "manual", &path, &error));
	CHECK_EQ(U32(ReadAll(path), 48), 2u);
}
