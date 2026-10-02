#include "Core/FileSystem.h"

#ifdef CAM_PLATFORM_WINDOWS

#include <assert.h>
#include <filesystem>
#include <iostream>
#include <Windows.h>
#include <shellapi.h>
#include <fileapi.h>
#include <Shlobj.h>

#include "Core/Log.h"

namespace Core
{
	namespace utils
	{
		void CALLBACK FileIOCompleteRoutine(DWORD errorCode, DWORD numberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
		{
		}

		static HANDLE OpenFileInternal(const std::string &path)
		{
			return CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
		}

		static bool ReadFileInternal(HANDLE file, void *buffer, int64 size)
		{
			OVERLAPPED ol = { 0 };
			return ReadFileEx(file, buffer, (DWORD)size, &ol, FileIOCompleteRoutine);
		}

		static int64 GetFileSizeInternal(HANDLE file)
		{
			LARGE_INTEGER size;
			GetFileSizeEx(file, &size);
			return size.QuadPart;
		}

		static std::string GetFolderById(const KNOWNFOLDERID &id)
		{
			PWSTR filePath;
			HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, NULL, &filePath);
			assert(result == S_OK);

			std::wstring filepathWstr = filePath;
			std::replace(filepathWstr.begin(), filepathWstr.end(), L'\\', L'/');
			std::filesystem::path resultPath = filepathWstr + L"/";
			return resultPath.string();
		}
	}
	
	int64 FileSystem::Seek(const std::string &filePath, int64 offset, int64 origin)
	{
		HANDLE handle = utils::OpenFileInternal(filePath);
		int64 result = 0;

		LARGE_INTEGER loff, lpos;
		loff.QuadPart = offset;
		if (handle && SetFilePointerEx(handle, loff, &lpos, (DWORD)origin))
		{
			result = lpos.QuadPart;
		}

		CloseHandle(handle);
		return result;
	}
	
	int64 FileSystem::Size(const std::string &filePath)
	{
		HANDLE handle = utils::OpenFileInternal(filePath);
		int64 result = 0;
		LARGE_INTEGER size;
		if (handle && GetFileSizeEx(handle, &size))
		{
			result = size.QuadPart;
		}

		CloseHandle(handle);
		return result;
	}

	uint32 FileSystem::ReadTextFile(const std::string &filePath, std::string *out_str)
	{
		HANDLE handle = utils::OpenFileInternal(filePath);
		if (handle == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			CAM_DEBUG_BREAK;
			std::cout << "File creation error: " << error << std::endl;
			return 0;
		}

		int64 size = utils::GetFileSizeInternal(handle);
		if (!out_str)
		{
			return 0;
		}

		char *readBuffer = new char[size + 1];
		readBuffer[size] = '\0';
		bool success = utils::ReadFileInternal(handle, &readBuffer[0], size);
		*out_str = std::string(readBuffer);
		delete[] readBuffer;

		CloseHandle(handle);
		return success;
	}

	bool FileSystem::WriteTextFile(const std::string &filePath, const std::string &str)
	{
		return WriteFile(filePath, (void*)&str[0], (uint32)str.size());
	}

	bool FileSystem::WriteFile(const std::string &filePath, void *src, uint32 bytes)
	{
		HANDLE file_handle = CreateFileA(filePath.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

		if (file_handle == INVALID_HANDLE_VALUE)
			return false;

		DWORD written;
		bool result = ::WriteFile(file_handle, src, (DWORD)bytes, &written, NULL);
		CloseHandle(file_handle);
		return result;
	}

	Byte *FileSystem::ReadFile(const std::string &filePath, uint32 *outSize)
	{
		if (!outSize)
		{
			return false;
		}

		HANDLE file_handle = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
		uint32 size = (uint32)utils::GetFileSizeInternal(file_handle);

		Byte *buffer = new Byte[size];
		memset(buffer, 0, size);

		bool result = utils::ReadFileInternal(file_handle, buffer, size);
		CloseHandle(file_handle);

		if (!result)
		{
			delete[] buffer;
			buffer = nullptr;
			return nullptr;
		}

		*outSize = size;

		return buffer;
	}
	
	uint32 FileSystem::Print(const std::string &filePath, const char *fmt, ...)
	{
		assert(fmt);

		va_list args;
		va_start(args, fmt);

		CHAR buf[4096];
		int32 res = vsnprintf(buf, sizeof(buf), fmt, args);
		if (res >= sizeof(buf))
		{
			// truncation
			res = sizeof(buf) - 1;
		}

		va_end(args);

		if (res <= 0)
		{
			return 0;
		}

		return WriteFile(filePath, buf, (uint32)res);
	}
	
	bool FileSystem::SetCurrentWorkingDirectory(const std::string &directory)
	{
		return ::SetCurrentDirectoryA(directory.c_str());
	}

	bool FileSystem::GetCurrentWorkingDirectory(std::string *out_directory)
	{
		CHAR buffer[MAX_PATH];
		DWORD dwRet;

		if (!out_directory)
		{
			return false;
		}

		dwRet = ::GetCurrentDirectoryA(MAX_PATH, buffer);

		if (dwRet == 0)
		{
			return false;
		}

		if (dwRet >= MAX_PATH)
		{
			return false;
		}

		buffer[dwRet] = '\0';
		*out_directory = std::string(buffer);
		return true;
	}
	bool FileSystem::DirectoryExists(const std::string &filePath) const
	{
		if (filePath.empty())
			return false;

		DWORD result = GetFileAttributesA(filePath.c_str());
		return (result != INVALID_FILE_ATTRIBUTES && (result & FILE_ATTRIBUTE_DIRECTORY));
	}

	bool FileSystem::FileExists(const std::string &filePath) const
	{
		if (filePath.empty())
			return false;

		// Any invalid result means, that the path does not exist. This includes ERROR_PATH_NOT_FOUND (a parent folder is missing), not only ERROR_FILE_NOT_FOUND.
		DWORD result = GetFileAttributesA(filePath.c_str());
		return result != INVALID_FILE_ATTRIBUTES;
	}
	
	bool FileSystem::RemoveFile(const std::string &filePath) const
	{
		return ::DeleteFileA(filePath.c_str());
	}
	
	bool FileSystem::RemoveDirectoy(const std::string &filePath) const
	{
		return RemoveDirectoryA(filePath.c_str());
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
		STARTUPINFOA startInfo;
		PROCESS_INFORMATION process;

		ZeroMemory(&startInfo, sizeof(startInfo));
		startInfo.cb = sizeof(STARTUPINFO);
		ZeroMemory(&process, sizeof(process));

		// The program expects to run in its own folder (like all programs of this project), not in the working directory of the caller.
		std::string workingDirectory = std::filesystem::path(executable).parent_path().string();

		if (!CreateProcessA(executable.c_str(), NULL, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, workingDirectory.empty() ? NULL : workingDirectory.c_str(), &startInfo, &process))
		{
			CAM_LOG_ERROR("Failed to start the process {}", executable);
			return false;
		}

		if (!CloseHandle(process.hProcess))
		{
			CAM_LOG_ERROR("Failed to close the handle for the process from {}", executable);
			return false;
		}

		if (!CloseHandle(process.hThread))
		{
			CAM_LOG_ERROR("Failed to close the handle for the thread from {}", executable);
			return false;
		}

		return true;
	}
}

#endif // CAM_PLATFORM_WINDOWS

