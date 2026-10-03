#pragma once

#include "Core.h"

#include <functional>
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
		/// The file of the running program.
		/// </summary>
		static std::string ExecutablePath();

		/// <summary>
		/// All programs are built into the same folder (bin/Debug or bin/Release in the root of the repository). A program, which runs from there, makes the folder
		/// of its project (for example CamServer, with the settings, the models, the known faces) its working directory, wherever it was started from.
		/// A program, which does not run from there (it was installed by the updater, or copied to a device), keeps its working directory.
		/// </summary>
		/// <param name="project">The name of the project folder, for example "CamServer".</param>
		/// <returns>Returns true, if the working directory was changed.</returns>
		static bool EnterProjectFolder(const std::string &project);

		/// <summary>
		/// True, if the process is still running.
		/// </summary>
		static bool IsRunning(uint32 pid);

		/// <summary>
		/// Ends a program: first it is asked to quit, so it can finish properly (close connections, finish files). If it is still there after the grace
		/// time, it is killed. On Linux it is asked with SIGTERM. On Windows with the stop request of the program, if it registered one
		/// (StartStopListener), which also works for a program without a console (a service, or a program started without a window), and with Ctrl+C
		/// otherwise (which needs a console).
		/// </summary>
		/// <param name="graceSeconds">How long the program has to quit on its own.</param>
		/// <returns>Returns true, if the program is gone.</returns>
		static bool Stop(uint32 pid, uint32 graceSeconds);

		/// <summary>
		/// Lets other programs ask this program to quit with Process::Stop(), without Ctrl+C or a console. On Windows this creates a named event for this
		/// process and waits for it on its own thread, the callback is called on that thread (like a signal handler, it should only tell the program to
		/// stop, and return). Call it once, at the start. Does nothing on Linux (returns true), where SIGTERM is used.
		/// </summary>
		/// <returns>Returns true, if the program can be asked now.</returns>
		static bool StartStopListener(std::function<void()> onStop);

		/// <summary>
		/// On Windows, a program cannot send Ctrl+C to another program without losing its own console. So Stop() starts this program again with a secret
		/// argument, which does that job. Every program, which uses Stop(), calls this at the very beginning of main(), and quits if it returns true.
		/// Does nothing (returns false) on other systems and without the argument.
		/// </summary>
		static bool HandleHelperCommand(int argc, char *argv[]);
	};
}
