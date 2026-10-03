#include "CamTest.h"
#include "TestUtils.h"

#include <cstring>
#include <thread>

TEST(Hash, Crc32OfTheCheckString)
{
	// The standard check value of CRC-32.
	const char *text = "123456789";
	CHECK_EQ(Core::Crc32(text, 9), 0xCBF43926u);
}

TEST(Hash, SameInputSameHashDifferentInputDifferentHash)
{
	CHECK_EQ(Core::Raw64("abc", 3), Core::Raw64("abc", 3));
	CHECK(Core::Raw64("abc", 3) != Core::Raw64("abd", 3));
	CHECK(Core::Mix32(1) != Core::Mix32(2));
	CHECK(Core::Mix64(1) != Core::Mix64(2));
}

TEST(RingBuffer, KeepsTheNewestElements)
{
	Core::RingBuffer<int> ring(3);
	for (int i = 1; i <= 5; ++i)
	{
		ring.Push(i);
	}

	// The oldest are dropped, when it is full.
	REQUIRE_EQ(ring.Size(), 3u);
	CHECK_EQ(ring.At(0), 3);
	CHECK_EQ(ring.At(1), 4);
	CHECK_EQ(ring.At(2), 5);
	CHECK_EQ(ring.Front(), 3);
}

TEST(RingBuffer, PopAndClear)
{
	Core::RingBuffer<int> ring(4);
	ring.Push(1);
	ring.Push(2);
	ring.Pop();
	CHECK_EQ(ring.Size(), 1u);
	CHECK_EQ(ring.Front(), 2);
	ring.Clear();
	CHECK_EQ(ring.Size(), 0u);
}

TEST(RingBuffer, WrapsAroundManyTimes)
{
	Core::RingBuffer<int> ring(5);
	for (int i = 0; i < 1000; ++i)
	{
		ring.Push(i);
	}

	REQUIRE_EQ(ring.Size(), 5u);
	for (uint32 i = 0; i < 5; ++i)
	{
		CHECK_EQ(ring.At(i), (int)(995 + i));
	}
}

TEST(ThreadSafeQueue, KeepsTheOrder)
{
	Core::ThreadSafeQueue<int> queue;
	queue.Enqueue(1);
	queue.Enqueue(2);
	CHECK_EQ(queue.Dequeue(), 1);
	CHECK_EQ(queue.Dequeue(), 2);
}

TEST(ThreadSafeQueue, TryDequeueTimesOutWhenEmpty)
{
	Core::ThreadSafeQueue<int> queue;
	int value = 0;
	CHECK(!queue.TryDequeue(value, 20));

	queue.Enqueue(5);
	CHECK(queue.TryDequeue(value, 20));
	CHECK_EQ(value, 5);
}

TEST(ThreadSafeQueue, DequeueWaitsForAnotherThread)
{
	Core::ThreadSafeQueue<int> queue;
	std::thread producer([&queue]
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		queue.Enqueue(42);
	});

	CHECK_EQ(queue.Dequeue(), 42);
	producer.join();
}

TEST(ThreadSafeQueue, ManyProducers)
{
	Core::ThreadSafeQueue<int> queue;
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t)
	{
		threads.emplace_back([&queue] { for (int i = 0; i < 250; ++i) queue.Enqueue(1); });
	}

	int sum = 0;
	for (int i = 0; i < 1000; ++i)
	{
		sum += queue.Dequeue();
	}

	for (std::thread &thread : threads)
	{
		thread.join();
	}

	CHECK_EQ(sum, 1000);
}

TEST(Buffer, AllocAndFree)
{
	Core::Buffer buffer;
	REQUIRE(buffer.Alloc(1024));
	CHECK_EQ(buffer.Size, 1024u);
	REQUIRE(buffer.Ptr != nullptr);
	memset(buffer.Ptr, 0xAB, buffer.Size);
	buffer.Free();
	CHECK(buffer.Ptr == nullptr);
	CHECK_EQ(buffer.Size, 0u);
}

TEST(Utils, ReadsMacrosFromText)
{
	std::string text = "#pragma once\n\n#define CAM_VERSION 123\n#define OTHER 5\n";
	CHECK(Core::utils::HasMacroInText(text, "CAM_VERSION"));
	CHECK(!Core::utils::HasMacroInText(text, "MISSING"));
	CHECK_EQ(Core::utils::GetMacroFromText(text, "CAM_VERSION"), "123");
	CHECK_EQ(Core::utils::GetMacroFromText(text, "MISSING"), "");
}

TEST(Utils, ReadsTheLocalVersionFromTheSource)
{
	TempDir dir;
	CHECK_EQ(Core::utils::GetLocalVersion(dir.Path()), 0u);		// no file

	std::filesystem::create_directories(dir.File("src"));
	Core::FileSystem::Get()->WriteTextFile(dir.File("src/CamVersion.h"), "#pragma once\n\n#define CAM_VERSION 77\n");
	CHECK_EQ(Core::utils::GetLocalVersion(dir.Path()), 77u);
}

TEST(Utils, MinMaxCount)
{
	CHECK_EQ(Core::utils::Min(3, 4), 3);
	CHECK_EQ(Core::utils::Max(3, 4), 4);
	int array[7];
	CHECK_EQ(Core::utils::Count(array), 7u);
}
