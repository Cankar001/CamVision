#include "Core/FileSystemWatcher.h"

#ifdef CAM_PLATFORM_LINUX

#include "Core/Core.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Core
{
	// Everything one running watcher thread needs. Shared between the thread and Stop(), so a stopped watcher can finish on its own
	// even when a new one was already started (e.g. SetWatchPath called from inside the callback).
	struct WatcherState
	{
		std::string Path;
		FileSystemWatcherCallbackFn Callback;
		int StopFd = -1;
		std::atomic<bool> Stop{ false };
		std::atomic<bool> Finished{ false };
		std::thread::id ThreadId;

		// The fd lives as long as the state, so Stop() never writes to an fd, which was closed (and maybe reused) in the meantime.
		~WatcherState()
		{
			if (StopFd >= 0)
			{
				close(StopFd);
			}
		}
	};

	static std::mutex s_Mutex;
	static std::shared_ptr<WatcherState> s_State;
	static std::atomic<bool> s_IgnoreNextChange{ false };
	static std::string s_WatchPath = "";
	static FileSystemWatcherCallbackFn s_Callback;

	// inotify does not watch recursively (like ReadDirectoryChangesW on Windows does), so every directory needs its own watch.
	static void AddWatchRecursive(int inotifyFd, std::map<int, std::string> &directories, const std::string &root, const std::string &relative)
	{
		static const uint32 mask = IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO;

		std::string fullPath = relative.empty() ? root : root + "/" + relative;
		int wd = inotify_add_watch(inotifyFd, fullPath.c_str(), mask);
		if (wd < 0)
		{
			return;
		}

		directories[wd] = relative;

		DIR *dir = opendir(fullPath.c_str());
		if (!dir)
		{
			return;
		}

		while (struct dirent *entry = readdir(dir))
		{
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			{
				continue;
			}

			std::string childRelative = relative.empty() ? std::string(entry->d_name) : relative + "/" + entry->d_name;
			struct stat info;
			if (stat((root + "/" + childRelative).c_str(), &info) == 0 && S_ISDIR(info.st_mode))
			{
				AddWatchRecursive(inotifyFd, directories, root, childRelative);
			}
		}

		closedir(dir);
	}

	void FileSystemWatcher::Start(const std::string &filePath, const FileSystemWatcherCallbackFn &callback)
	{
		Stop();

		auto state = std::make_shared<WatcherState>();
		state->Path = filePath;
		state->Callback = callback;
		state->StopFd = eventfd(0, EFD_CLOEXEC);
		if (state->StopFd < 0)
		{
			return;
		}

		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			s_WatchPath = filePath;
			s_Callback = callback;
			s_State = state;
		}

		// The thread is detached and owns its state, so the process can always exit, even if Stop() was never called.
		std::thread([state]()
		{
			state->ThreadId = std::this_thread::get_id();
			FileSystemWatcher::Watch(state.get());
			state->Finished = true;
		}).detach();
	}

	void FileSystemWatcher::Stop()
	{
		std::shared_ptr<WatcherState> state;
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			state = s_State;
			s_State.reset();
		}

		if (!state || state->Finished)
		{
			return;
		}

		state->Stop = true;
		uint64 one = 1;
		ssize_t written = write(state->StopFd, &one, sizeof(one));
		(void)written;

		// Wait until the thread is done, so no callback is running anymore after Stop() returns. A callback, which stops its own watcher, can not wait for itself.
		if (state->ThreadId != std::this_thread::get_id())
		{
			for (int i = 0; i < 1000 && !state->Finished; ++i)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}

	void FileSystemWatcher::SetWatchPath(const std::string &filePath)
	{
		FileSystemWatcherCallbackFn callback;
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			s_WatchPath = filePath;
			callback = s_Callback;
		}

		Stop();
		Start(filePath, callback);
	}

	void FileSystemWatcher::DisableWatchUntilNextAction()
	{
		s_IgnoreNextChange = true;
	}

	unsigned long FileSystemWatcher::Watch(void *param)
	{
		WatcherState *state = (WatcherState *)param;

		int inotifyFd = inotify_init1(IN_CLOEXEC);
		if (inotifyFd < 0)
		{
			return 0ul;
		}

		std::map<int, std::string> directories;
		AddWatchRecursive(inotifyFd, directories, state->Path, "");
		if (directories.empty())
		{
			// Like on Windows: nothing to watch, if the directory can not be opened.
			close(inotifyFd);
			return 0ul;
		}

		alignas(struct inotify_event) char buffer[16384];
		struct pollfd fds[2];
		fds[0].fd = inotifyFd;
		fds[0].events = POLLIN;
		fds[1].fd = state->StopFd;
		fds[1].events = POLLIN;

		while (!state->Stop)
		{
			int ready = poll(fds, 2, -1);
			if (ready < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}

				break;
			}

			if (fds[1].revents & POLLIN)
			{
				break;
			}

			if (!(fds[0].revents & POLLIN))
			{
				continue;
			}

			ssize_t length = read(inotifyFd, buffer, sizeof(buffer));
			if (length <= 0)
			{
				if (length < 0 && (errno == EINTR || errno == EAGAIN))
				{
					continue;
				}

				break;
			}

			std::vector<FileSystemWatcherContext> contexts;

			// A rename arrives as IN_MOVED_FROM followed by IN_MOVED_TO with the same cookie.
			bool hasPending = false;
			uint32 pendingCookie = 0;
			std::string pendingName;

			auto flushPending = [&]()
			{
				if (hasPending)
				{
					FileSystemWatcherContext context = {};
					context.Action = FileSystemWatcherAction::Removed;
					context.FilePath = pendingName;
					contexts.push_back(context);
					hasPending = false;
				}
			};

			for (char *ptr = buffer; ptr < buffer + length;)
			{
				const struct inotify_event *event = (const struct inotify_event *)ptr;
				ptr += sizeof(struct inotify_event) + event->len;

				if (event->mask & IN_IGNORED)
				{
					directories.erase(event->wd);
					continue;
				}

				auto directory = directories.find(event->wd);
				if (directory == directories.end() || event->len == 0)
				{
					continue;
				}

				std::string name = directory->second.empty() ? std::string(event->name) : directory->second + "/" + event->name;

				if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)))
				{
					AddWatchRecursive(inotifyFd, directories, state->Path, name);
				}

				if (event->mask & IN_MOVED_FROM)
				{
					flushPending();
					hasPending = true;
					pendingCookie = event->cookie;
					pendingName = name;
					continue;
				}

				if (event->mask & IN_MOVED_TO)
				{
					FileSystemWatcherContext context = {};
					if (hasPending && pendingCookie == event->cookie)
					{
						context.Action = FileSystemWatcherAction::Renamed;
						context.OldName = pendingName;
						hasPending = false;
					}
					else
					{
						flushPending();
						context.Action = FileSystemWatcherAction::Added;
					}

					context.FilePath = name;
					contexts.push_back(context);
					continue;
				}

				flushPending();

				FileSystemWatcherContext context = {};
				context.FilePath = name;
				if (event->mask & IN_CREATE)
				{
					context.Action = FileSystemWatcherAction::Added;
				}
				else if (event->mask & IN_DELETE)
				{
					context.Action = FileSystemWatcherAction::Removed;
				}
				else if (event->mask & IN_CLOSE_WRITE)
				{
					context.Action = FileSystemWatcherAction::Modified;
				}
				else
				{
					continue;
				}

				contexts.push_back(context);
			}

			flushPending();

			if (contexts.empty())
			{
				continue;
			}

			if (s_IgnoreNextChange.exchange(false))
			{
				continue;
			}

			for (const FileSystemWatcherContext &context : contexts)
			{
				if (state->Stop)
				{
					break;
				}

				if (state->Callback)
				{
					state->Callback(context);
				}
			}
		}

		close(inotifyFd);
		return 0ul;
	}
}

#endif // CAM_PLATFORM_LINUX
