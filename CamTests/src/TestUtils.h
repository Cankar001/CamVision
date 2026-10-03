#pragma once

#include <Cam-Core.h>

#include <filesystem>
#include <random>
#include <string>

// An empty folder in the temp folder of the system, which is deleted again at the end of the scope.
class TempDir
{
public:

	TempDir()
	{
		std::random_device random;
		m_Path = std::filesystem::temp_directory_path() / ("camtests_" + std::to_string(random()) + "_" + std::to_string(random()));
		std::filesystem::create_directories(m_Path);
	}

	~TempDir()
	{
		std::error_code error;
		std::filesystem::remove_all(m_Path, error);
	}

	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	// The folder, with forward slashes (the project uses them everywhere).
	std::string Path() const { return m_Path.generic_string(); }

	std::string File(const std::string &name) const { return (m_Path / name).generic_string(); }

private:

	std::filesystem::path m_Path;
};

// The file name of the running test program (this is how the program is started as a stand-in for a CamClient, see Main.cpp).
std::string TestProgramPath();

// The name, which the test program has, when it is started as a stand-in for another program: it only waits.
constexpr const char *SLEEPER_NAME = "camtests_sleeper";
