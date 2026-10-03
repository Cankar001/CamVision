#include "CamTest.h"
#include "TestUtils.h"

#include <chrono>
#include <thread>

#ifdef CAM_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

std::string TestProgramPath()
{
#ifdef CAM_PLATFORM_WINDOWS
	char path[MAX_PATH * 2];
	DWORD length = GetModuleFileNameA(nullptr, path, (DWORD)sizeof(path));
	return std::string(path, length);
#else
	char path[PATH_MAX];
	ssize_t length = readlink("/proc/self/exe", path, sizeof(path));
	return length > 0 ? std::string(path, (size_t)length) : std::string();
#endif
}

int main(int argc, char *argv[])
{
	// On Windows, stopping a program uses this program again as a small helper.
	if (Core::Process::HandleHelperCommand(argc, argv))
	{
		return 0;
	}

	// Tests of the process handling start a copy of this program as a stand-in for a running program (the file name is the trigger,
	// because the program is started without arguments). It does nothing but wait, until it is stopped.
	if (std::filesystem::path(TestProgramPath()).stem().string() == SLEEPER_NAME)
	{
		std::this_thread::sleep_for(std::chrono::minutes(2));
		return 0;
	}

	Core::Logger::SetSilent(true);
	Core::Init();
	int result = CamTest::Run(argc, argv);
	Core::Shutdown();
	return result;
}
