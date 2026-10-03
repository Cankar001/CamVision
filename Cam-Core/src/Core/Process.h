#pragma once

#include "Core.h"

#include <string>
#include <vector>

namespace Core
{
	/// <summary>
	/// Finding and ending other programs, which are running on this computer.
	/// </summary>
	class Process
	{
	public:

		/// <summary>
		/// Finds the running programs, which were started from exactly this executable file (the file itself, not just a program with the same name
		/// somewhere else). The own process is never in the list.
		/// </summary>
		static std::vector<uint32> FindByExecutable(const std::string &path);

		/// <summary>
		/// True, if the process is still running.
		/// </summary>
		static bool IsRunning(uint32 pid);

		/// <summary>
		/// Ends a program: first it is asked to quit (SIGTERM on Linux, Ctrl+C on Windows), so it can finish properly (close connections, finish files).
		/// If it is still there after the grace time, it is killed.
		/// </summary>
		/// <param name="graceSeconds">How long the program has to quit on its own.</param>
		/// <returns>Returns true, if the program is gone.</returns>
		static bool Stop(uint32 pid, uint32 graceSeconds);

		/// <summary>
		/// On Windows, a program cannot send Ctrl+C to another program without losing its own console. So Stop() starts this program again with a secret
		/// argument, which does that job. Every program, which uses Stop(), calls this at the very beginning of main(), and quits if it returns true.
		/// Does nothing (returns false) on other systems and without the argument.
		/// </summary>
		static bool HandleHelperCommand(int argc, char *argv[]);
	};
}
