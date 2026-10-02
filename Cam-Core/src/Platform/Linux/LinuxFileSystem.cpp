#include "Core/FileSystem.h"

#ifdef CAM_PLATFORM_LINUX

#include <assert.h>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Core/Log.h"

namespace Core
{
	int64 FileSystem::Seek(const std::string &filePath, int64 offset, int64 origin)
	{
		// origin uses the same values as on Windows (FILE_BEGIN, FILE_CURRENT, FILE_END), which are SEEK_SET, SEEK_CUR and SEEK_END.
		FILE *f = fopen(filePath.c_str(), "rb");
		if (!f)
		{
			return 0;
		}

		int64 result = 0;
		if (fseeko(f, (off_t)offset, (int)origin) == 0)
		{
			result = (int64)ftello(f);
		}

		fclose(f);
		return result;
	}

	int64 FileSystem::Size(const std::string &filePath)
	{
		struct stat info;
		if (stat(filePath.c_str(), &info) == 0 && S_ISREG(info.st_mode))
		{
			return (int64)info.st_size;
		}

		return 0;
	}

	uint32 FileSystem::ReadTextFile(const std::string &filePath, std::string *out_str)
	{
		if (!out_str)
		{
			std::cout << "Needed valid pointer to the result string!" << std::endl;
			return 0;
		}

		FILE *f = fopen(filePath.c_str(), "rb");
		if (!f)
		{
			std::cout << "Could not open file " << filePath.c_str() << std::endl;
			return 0;
		}

		fseek(f, 0, SEEK_END);
		long length = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (length < 0)
		{
			fclose(f);
			return 0;
		}

		std::string content((size_t)length, '\0');
		size_t read = length > 0 ? fread(&content[0], 1, (size_t)length, f) : 0;
		fclose(f);

		content.resize(read);
		*out_str = content;
		return (uint32)read;
	}

	bool FileSystem::WriteTextFile(const std::string &filePath, const std::string &str)
	{
		return WriteFile(filePath, (void*)str.data(), (uint32)str.size());
	}

	bool FileSystem::WriteFile(const std::string &filePath, void *src, uint32 bytes)
	{
		// Unlike the Windows implementation (which refuses to touch existing files), this overwrites existing files.
		FILE *f = fopen(filePath.c_str(), "wb");
		if (!f)
		{
			return false;
		}

		bool success = (bytes == 0) || (fwrite(src, 1, bytes, f) == bytes);
		success = (fclose(f) == 0) && success;
		return success;
	}

	Byte *FileSystem::ReadFile(const std::string &filePath, uint32 *outSize)
	{
		if (!outSize)
		{
			return nullptr;
		}

		FILE *f = fopen(filePath.c_str(), "rb");
		if (!f)
		{
			return nullptr;
		}

		fseek(f, 0, SEEK_END);
		long length = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (length < 0)
		{
			fclose(f);
			return nullptr;
		}

		// Allocated with new[] like on Windows, the caller releases it with delete[].
		Byte *buffer = new Byte[(size_t)length + 1];
		size_t read = length > 0 ? fread(buffer, 1, (size_t)length, f) : 0;
		fclose(f);

		if (read != (size_t)length)
		{
			delete[] buffer;
			return nullptr;
		}

		buffer[length] = 0;
		*outSize = (uint32)length;
		return buffer;
	}

	uint32 FileSystem::Print(const std::string &filePath, const char *fmt, ...)
	{
		assert(fmt);

		va_list args;
		va_start(args, fmt);

		char buf[4096];
		int32 res = vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);

		if (res >= (int32)sizeof(buf))
		{
			// truncation
			res = sizeof(buf) - 1;
		}

		if (res <= 0)
		{
			return 0;
		}

		return WriteFile(filePath, buf, (uint32)res);
	}

	bool FileSystem::SetCurrentWorkingDirectory(const std::string &directory)
	{
		if (chdir(directory.c_str()) == -1)
		{
			printf("Error %s\n", strerror(errno));
			return false;
		}

		return true;
	}

	bool FileSystem::GetCurrentWorkingDirectory(std::string *out_directory)
	{
		if (!out_directory)
		{
			return false;
		}

		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd)) != NULL)
		{
			*out_directory = std::string(cwd);
			return true;
		}

		return false;
	}

	bool FileSystem::DirectoryExists(const std::string &filePath) const
	{
		if (filePath.empty())
		{
			return false;
		}

		struct stat info;
		return stat(filePath.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
	}

	bool FileSystem::FileExists(const std::string &filePath) const
	{
		// Like on Windows, this is true for any existing path.
		if (filePath.empty())
		{
			return false;
		}

		struct stat info;
		return stat(filePath.c_str(), &info) == 0;
	}

	bool FileSystem::RemoveFile(const std::string &filePath) const
	{
		return unlink(filePath.c_str()) == 0;
	}

	bool FileSystem::RemoveDirectoy(const std::string &filePath) const
	{
		return rmdir(filePath.c_str()) == 0;
	}

	bool FileSystem::MakeDirectory(const std::string &filePath) const
	{
		if (filePath.empty())
		{
			return false;
		}

		std::error_code error;
		std::filesystem::create_directories(filePath, error);
		return !error && DirectoryExists(filePath);
	}

	bool FileSystem::StartProgram(const std::string &executable)
	{
		if (access(executable.c_str(), X_OK) != 0)
		{
			CAM_LOG_ERROR("Failed to start the process {}: {}", executable, strerror(errno));
			return false;
		}

		// The program expects to run in its own folder (like all programs of this project), so it needs an absolute path, which stays valid after changing the directory.
		char resolved[PATH_MAX];
		if (!realpath(executable.c_str(), resolved))
		{
			CAM_LOG_ERROR("Failed to start the process {}: {}", executable, strerror(errno));
			return false;
		}

		std::string absolutePath = resolved;
		std::string workingDirectory = std::filesystem::path(absolutePath).parent_path().string();

		// Double fork, so the started program is detached from this process (it keeps running when we exit) and never becomes a zombie.
		pid_t pid = fork();
		if (pid < 0)
		{
			CAM_LOG_ERROR("Failed to start the process {}: fork failed", executable);
			return false;
		}

		if (pid == 0)
		{
			setsid();

			pid_t second = fork();
			if (second != 0)
			{
				_exit(second < 0 ? 1 : 0);
			}

			if (chdir(workingDirectory.c_str()) != 0)
			{
				_exit(126);
			}

			char *argv[] = { (char *)absolutePath.c_str(), nullptr };
			execv(absolutePath.c_str(), argv);
			_exit(127);
		}

		int status = 0;
		if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			CAM_LOG_ERROR("Failed to start the process {}", executable);
			return false;
		}

		return true;
	}
}

#endif // CAM_PLATFORM_LINUX
