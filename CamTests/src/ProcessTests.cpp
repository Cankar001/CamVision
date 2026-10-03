#include "CamTest.h"
#include "TestUtils.h"

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef CAM_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace
{
	// A copy of the test program with another name. It starts and waits (see Main.cpp), so it is a stand-in for a running CamClient.
	// It is always stopped at the end of the scope, so a failing test leaves nothing behind.
	class Sleeper
	{
	public:

		Sleeper()
		{
			std::string extension = std::filesystem::path(TestProgramPath()).extension().string();
			m_File = m_Dir.File(std::string(SLEEPER_NAME) + extension);
			std::filesystem::copy_file(TestProgramPath(), m_File);
		}

		~Sleeper()
		{
			for (uint32 pid : Core::Process::FindByExecutable(m_File))
			{
				Core::Process::Stop(pid, 1);
			}
		}

		bool Start()
		{
#ifdef CAM_PLATFORM_WINDOWS
			// In its own console, as a CamClient on a device has one. Ctrl+C is sent to a console, so the test program must not share its own.
			STARTUPINFOA startup = {};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process = {};
			std::string command = "\"" + m_File + "\"";
			if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process))
			{
				return false;
			}

			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			return true;
#else
			return Core::FileSystem::Get()->StartProgram(m_File);
#endif
		}

		// Waits until the program is running (it needs a moment to start).
		std::vector<uint32> WaitForStart()
		{
			for (int i = 0; i < 100; ++i)
			{
				std::vector<uint32> pids = Core::Process::FindByExecutable(m_File);
				if (!pids.empty())
				{
					// A program, which was started a moment ago, is not ready to receive Ctrl+C yet.
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
					return pids;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}

			return {};
		}

		const std::string &File() const { return m_File; }

	private:

		TempDir m_Dir;
		std::string m_File;
	};
}

TEST(Process, FindsNothingForAProgramThatIsNotRunning)
{
	TempDir dir;
	CHECK(Core::Process::FindByExecutable(dir.File("not_running.exe")).empty());
}

TEST(Process, NeverReturnsTheOwnProcess)
{
	CHECK(Core::Process::FindByExecutable(TestProgramPath()).empty());
}

TEST(Process, FindsARunningProgramByItsFile)
{
	Sleeper sleeper;
	REQUIRE(sleeper.Start());

	std::vector<uint32> pids = sleeper.WaitForStart();
	REQUIRE_EQ(pids.size(), (size_t)1);
	CHECK(Core::Process::IsRunning(pids[0]));
}

TEST(Process, ProgramsWithTheSameNameInOtherFoldersAreNotFound)
{
	Sleeper sleeper;
	REQUIRE(sleeper.Start());
	REQUIRE(!sleeper.WaitForStart().empty());

	// The same file name, but not the file, which was started (for example the CamClient of another installation).
	std::string other = TempDir().File(std::filesystem::path(sleeper.File()).filename().string());
	CHECK(Core::Process::FindByExecutable(other).empty());
}

TEST(Process, StopEndsTheProgram)
{
	Sleeper sleeper;
	REQUIRE(sleeper.Start());

	std::vector<uint32> pids = sleeper.WaitForStart();
	REQUIRE_EQ(pids.size(), (size_t)1);

	auto start = std::chrono::steady_clock::now();
	CHECK(Core::Process::Stop(pids[0], 5));
	CHECK(!Core::Process::IsRunning(pids[0]));
	CHECK(Core::Process::FindByExecutable(sleeper.File()).empty());

	// It was asked to quit, or killed after the grace time, but it never takes much longer than that.
	auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
	CHECK(seconds < 30);

#ifndef CAM_PLATFORM_WINDOWS
	// SIGTERM ends the stand-in at once, it is not killed after the grace time. (On Windows, Ctrl+C needs a console, which a build machine without a
	// desktop might not give, then the program is killed after the grace time. That is fine for the test, so it is not checked there.)
	CHECK(seconds < 4);
#endif
}

TEST(Process, StoppingAProgramThatIsGoneIsFine)
{
	Sleeper sleeper;
	REQUIRE(sleeper.Start());

	std::vector<uint32> pids = sleeper.WaitForStart();
	REQUIRE_EQ(pids.size(), (size_t)1);
	REQUIRE(Core::Process::Stop(pids[0], 5));

	CHECK(Core::Process::Stop(pids[0], 1));
}

TEST(Process, OtherProgramsStayUntouched)
{
	Sleeper first, second;
	REQUIRE(first.Start());
	REQUIRE(second.Start());

	std::vector<uint32> first_pids = first.WaitForStart();
	std::vector<uint32> second_pids = second.WaitForStart();
	REQUIRE_EQ(first_pids.size(), (size_t)1);
	REQUIRE_EQ(second_pids.size(), (size_t)1);

	REQUIRE(Core::Process::Stop(first_pids[0], 5));
	CHECK(Core::Process::IsRunning(second_pids[0]));
}
