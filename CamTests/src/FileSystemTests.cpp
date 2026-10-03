#include "CamTest.h"
#include "TestUtils.h"

#include <cstring>

TEST(FileSystem, WriteAndReadBinaryFile)
{
	TempDir dir;
	Core::FileSystem *fs = Core::FileSystem::Get();

	std::vector<Byte> data(5000);
	for (size_t i = 0; i < data.size(); ++i)
	{
		data[i] = (Byte)(i * 31);
	}

	std::string file = dir.File("data.bin");
	REQUIRE(fs->WriteFile(file, data.data(), (uint32)data.size()));
	CHECK(fs->FileExists(file));
	CHECK_EQ(fs->Size(file), (int64)data.size());

	uint32 size = 0;
	Byte *read = fs->ReadFile(file, &size);
	REQUIRE(read != nullptr);
	REQUIRE_EQ(size, (uint32)data.size());
	CHECK(memcmp(read, data.data(), size) == 0);
	delete[] read;
}

TEST(FileSystem, WriteAndReadTextFile)
{
	TempDir dir;
	Core::FileSystem *fs = Core::FileSystem::Get();

	std::string file = dir.File("text.txt");
	REQUIRE(fs->WriteTextFile(file, "hello\nworld"));

	std::string content;
	CHECK(fs->ReadTextFile(file, &content) > 0);
	CHECK_EQ(content, "hello\nworld");
}

TEST(FileSystem, FileExistsOnlyForFiles)
{
	TempDir dir;
	Core::FileSystem *fs = Core::FileSystem::Get();

	CHECK(!fs->FileExists(dir.File("missing.txt")));
	CHECK(!fs->FileExists(dir.File("missing_folder/missing.txt")));		// the folder is missing too

	fs->WriteTextFile(dir.File("there.txt"), "x");
	CHECK(fs->FileExists(dir.File("there.txt")));
}

TEST(FileSystem, MakeAndRemoveDirectories)
{
	TempDir dir;
	Core::FileSystem *fs = Core::FileSystem::Get();

	std::string folder = dir.File("sub");
	CHECK(!fs->DirectoryExists(folder));
	REQUIRE(fs->MakeDirectory(folder));
	CHECK(fs->DirectoryExists(folder));

	// Making a folder, which exists already, is fine (the updater does it before every update).
	CHECK(fs->MakeDirectory(folder));

	CHECK(fs->RemoveDirectoy(folder));
	CHECK(!fs->DirectoryExists(folder));
}

TEST(FileSystem, RemoveFile)
{
	TempDir dir;
	Core::FileSystem *fs = Core::FileSystem::Get();

	std::string file = dir.File("remove_me.txt");
	fs->WriteTextFile(file, "x");
	REQUIRE(fs->FileExists(file));
	CHECK(fs->RemoveFile(file));
	CHECK(!fs->FileExists(file));
}

TEST(FileSystem, ChangesTheWorkingDirectory)
{
	TempDir dir;
	Core::FileSystem *fs = Core::FileSystem::Get();

	std::string before;
	REQUIRE(fs->GetCurrentWorkingDirectory(&before));

	REQUIRE(fs->SetCurrentWorkingDirectory(dir.Path()));
	fs->WriteTextFile("relative.txt", "x");
	CHECK(fs->FileExists(dir.File("relative.txt")));

	// Leave the folder, so it can be deleted.
	CHECK(fs->SetCurrentWorkingDirectory(before));
}
