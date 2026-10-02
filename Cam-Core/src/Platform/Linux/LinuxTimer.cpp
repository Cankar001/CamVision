#include "Core/Timer.h"

#ifdef CAM_PLATFORM_LINUX

#include <chrono>
#include <thread>

namespace Core
{
	int64 QueryMS()
	{
		// Monotonic clock, counts from boot like GetTickCount64 on Windows.
		auto now = std::chrono::steady_clock::now().time_since_epoch();
		return (int64)std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	}

	void SleepMS(uint32 ms)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	}
}

#endif // CAM_PLATFORM_LINUX
