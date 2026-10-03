#include "Core/Process.h"

#ifdef CAM_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace Core
{
	namespace
	{
		// The same file is written differently (upper and lower case, 8.3 names, relative paths), so paths are compared in one canonical form.
		std::wstring Canonical(const std::filesystem::path &path)
		{
			std::error_code error;
			std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
			std::wstring text = (error ? path : canonical).wstring();
			std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
			return text;
		}

		const char *HELPER_ARGUMENT = "--send-ctrl-c=";

		// If this program shares its console with the program, which is stopped (for example because it was started by it), the Ctrl+C reaches this
		// program too. It is swallowed while stopping.
		BOOL WINAPI SwallowCtrlC(DWORD type)
		{
			return type == CTRL_C_EVENT;
		}

		// The event, with which a program is asked to quit. One per process, in the global namespace, so a program in another session (a service) is found
		// too.
		std::wstring StopEventName(uint32 pid)
		{
			return L"Global\\CamVision.Stop." + std::to_wstring(pid);
		}
	}

	std::vector<uint32> Process::FindByExecutable(const std::string &path)
	{
		std::vector<uint32> result;

		std::filesystem::path target(path);
		std::wstring file_name = target.filename().wstring();
		std::wstring target_path = Canonical(std::filesystem::absolute(target));

		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			return result;
		}

		PROCESSENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
		{
			if (_wcsicmp(entry.szExeFile, file_name.c_str()) != 0 || entry.th32ProcessID == GetCurrentProcessId())
			{
				continue;
			}

			// The name is the same, is it the same file?
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (!process)
			{
				continue;
			}

			wchar_t image[MAX_PATH * 4];
			DWORD length = (DWORD)(sizeof(image) / sizeof(image[0]));
			if (QueryFullProcessImageNameW(process, 0, image, &length) && Canonical(std::filesystem::path(image)) == target_path)
			{
				result.push_back((uint32)entry.th32ProcessID);
			}

			CloseHandle(process);
		}

		CloseHandle(snapshot);
		return result;
	}

	std::string Process::ExecutablePath()
	{
		char path[MAX_PATH * 4];
		DWORD length = GetModuleFileNameA(nullptr, path, (DWORD)sizeof(path));
		return std::string(path, length);
	}

	bool Process::IsRunning(uint32 pid)
	{
		HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (!process)
		{
			// Not there anymore (or not accessible, which is not the case for our own programs).
			return false;
		}

		bool running = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
		CloseHandle(process);
		return running;
	}

	bool Process::Stop(uint32 pid, uint32 graceSeconds)
	{
		HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
		if (!process)
		{
			return !IsRunning(pid);
		}

		// Ask politely. A program, which registered a stop request (StartStopListener), is asked with it, which works without a console too.
		bool asked = false;
		HANDLE stop_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, StopEventName(pid).c_str());
		if (stop_event)
		{
			asked = SetEvent(stop_event) != FALSE;
			CloseHandle(stop_event);
		}

		// Otherwise a helper (this program again) sends Ctrl+C to the console of the other program.
		SetConsoleCtrlHandler(SwallowCtrlC, TRUE);
		char self[MAX_PATH * 2];
		if (!asked && GetModuleFileNameA(nullptr, self, (DWORD)sizeof(self)) > 0)
		{
			std::string command = "\"" + std::string(self) + "\" " + HELPER_ARGUMENT + std::to_string(pid);
			std::vector<char> mutable_command(command.begin(), command.end());
			mutable_command.push_back('\0');

			STARTUPINFOA startup = {};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION helper = {};
			if (CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &helper))
			{
				WaitForSingleObject(helper.hProcess, 10000);
				CloseHandle(helper.hProcess);
				CloseHandle(helper.hThread);
			}
		}

		bool gone = WaitForSingleObject(process, graceSeconds * 1000) == WAIT_OBJECT_0;
		if (!gone)
		{
			// It did not quit on its own.
			TerminateProcess(process, 1);
			gone = WaitForSingleObject(process, 10000) == WAIT_OBJECT_0;
		}

		SetConsoleCtrlHandler(SwallowCtrlC, FALSE);
		CloseHandle(process);
		return gone;
	}

	bool Process::StartStopListener(std::function<void()> onStop)
	{
		static HANDLE s_Event = nullptr;
		if (s_Event)
		{
			return true;
		}

		// A manual reset event, which is set once and stays set: the request is not lost, if it comes before the thread waits.
		s_Event = CreateEventW(nullptr, TRUE, FALSE, StopEventName(GetCurrentProcessId()).c_str());
		if (!s_Event)
		{
			return false;
		}

		HANDLE stop_event = s_Event;
		std::thread([stop_event, onStop]
		{
			if (WaitForSingleObject(stop_event, INFINITE) == WAIT_OBJECT_0 && onStop)
			{
				onStop();
			}
		}).detach();

		return true;
	}

	bool Process::HandleHelperCommand(int argc, char *argv[])
	{
		for (int i = 1; i < argc; ++i)
		{
			if (strncmp(argv[i], HELPER_ARGUMENT, strlen(HELPER_ARGUMENT)) != 0)
			{
				continue;
			}

			DWORD pid = (DWORD)atoi(argv[i] + strlen(HELPER_ARGUMENT));

			// This helper has a console of its own (hidden), which has to be left first. Then it joins the console of the other program, sends Ctrl+C to
			// everybody who is on it (only the other program), and leaves again. The helper ignores the Ctrl+C itself.
			FreeConsole();
			if (pid != 0 && AttachConsole(pid))
			{
				SetConsoleCtrlHandler(nullptr, TRUE);
				GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
				Sleep(500);
				FreeConsole();
			}

			return true;
		}

		return false;
	}
}

#endif // CAM_PLATFORM_WINDOWS
