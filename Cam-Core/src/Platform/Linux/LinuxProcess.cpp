#include "Core/Process.h"

#ifdef CAM_PLATFORM_LINUX

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace Core
{
	std::vector<uint32> Process::FindByExecutable(const std::string &path)
	{
		std::vector<uint32> result;

		std::error_code error;
		std::filesystem::path target = std::filesystem::weakly_canonical(std::filesystem::absolute(path), error);
		if (error)
		{
			return result;
		}

		for (std::filesystem::directory_iterator it("/proc", error), end; !error && it != end; it.increment(error))
		{
			std::string name = it->path().filename().string();
			if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
			{
				continue;
			}

			uint32 pid = (uint32)strtoul(name.c_str(), nullptr, 10);
			if (pid == (uint32)getpid())
			{
				continue;
			}

			// /proc/<pid>/exe is the file, from which the program was started. If the file was replaced since then (by an update), it says " (deleted)".
			std::error_code link_error;
			std::filesystem::path exe = std::filesystem::read_symlink(it->path() / "exe", link_error);
			if (link_error)
			{
				continue;
			}

			std::string text = exe.string();
			const std::string deleted = " (deleted)";
			if (text.size() > deleted.size() && text.compare(text.size() - deleted.size(), deleted.size(), deleted) == 0)
			{
				text.resize(text.size() - deleted.size());
			}

			if (std::filesystem::path(text) == target)
			{
				result.push_back(pid);
			}
		}

		return result;
	}

	bool Process::IsRunning(uint32 pid)
	{
		if (kill((pid_t)pid, 0) != 0 && errno != EPERM)
		{
			return false;
		}

		// A program, which has ended but was not collected by its parent yet (a zombie), is not running anymore.
		std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
		std::string line;
		if (std::getline(stat, line))
		{
			size_t close = line.rfind(')');
			if (close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z')
			{
				return false;
			}
		}

		return true;
	}

	bool Process::Stop(uint32 pid, uint32 graceSeconds)
	{
		if (!IsRunning(pid))
		{
			return true;
		}

		// Ask politely: the programs of this project finish properly on SIGTERM.
		kill((pid_t)pid, SIGTERM);
		for (uint32 waited = 0; waited < graceSeconds * 10 && IsRunning(pid); ++waited)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		if (IsRunning(pid))
		{
			// It did not quit on its own.
			kill((pid_t)pid, SIGKILL);
			for (int waited = 0; waited < 100 && IsRunning(pid); ++waited)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}

		return !IsRunning(pid);
	}

	bool Process::HandleHelperCommand(int argc, char *argv[])
	{
		// Not needed on Linux, SIGTERM can be sent directly.
		(void)argc;
		(void)argv;
		return false;
	}
}

#endif // CAM_PLATFORM_LINUX
