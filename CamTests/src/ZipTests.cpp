#include "CamTest.h"
#include "TestUtils.h"

#include "Utils/ZipArchive.h"

#include <cstring>

static Core::ZipFile MakeFile(const std::string &name, std::vector<Byte> &data)
{
	Core::ZipFile file;
	file.Name = name;
	file.Buffer = data.data();
	file.BufferSize = data.size();
	return file;
}

TEST(Zip, StoreAndLoadRoundTrip)
{
	TempDir dir;

	std::vector<Byte> first(100000);
	for (size_t i = 0; i < first.size(); ++i)
	{
		first[i] = (Byte)((i * 7) ^ (i >> 8));
	}

	std::vector<Byte> second = { 'h', 'i' };
	std::vector<Byte> third;	// an empty file

	Core::ZipArchive archive;
	std::string path = dir.File("archive.zip");
	REQUIRE(archive.Store({ MakeFile("a.bin", first), MakeFile("sub/b.txt", second), MakeFile("empty.txt", third) }, path));
	REQUIRE(Core::FileSystem::Get()->FileExists(path));

	std::vector<Core::ZipFile> loaded = archive.Load(path);
	REQUIRE(loaded.size() >= 2);

	bool found_first = false, found_second = false;
	for (Core::ZipFile &file : loaded)
	{
		if (file.Name == "a.bin")
		{
			found_first = true;
			REQUIRE_EQ(file.BufferSize, first.size());
			CHECK(memcmp(file.Buffer, first.data(), first.size()) == 0);
		}
		else if (file.Name == "sub/b.txt")
		{
			found_second = true;
			REQUIRE_EQ(file.BufferSize, second.size());
			CHECK(memcmp(file.Buffer, second.data(), second.size()) == 0);
		}

		delete[] (Byte *)file.Buffer;
	}

	CHECK(found_first);
	CHECK(found_second);
}

TEST(Zip, LoadingAMissingOrBrokenArchiveGivesNothing)
{
	TempDir dir;
	Core::ZipArchive archive;
	CHECK(archive.Load(dir.File("missing.zip")).empty());

	Core::FileSystem::Get()->WriteTextFile(dir.File("broken.zip"), "this is not a zip archive at all");
	CHECK(archive.Load(dir.File("broken.zip")).empty());
}
