#include "Core/Process.h"

#include <filesystem>

namespace Core
{
	bool Process::EnterProjectFolder(const std::string &project)
	{
		std::string executable = ExecutablePath();
		if (executable.empty())
		{
			return false;
		}

		// The layout is <root>/bin/<Configuration>/<program>, or one folder deeper for the files, which are put together for the updater
		// (bin/<Configuration>/CamClient-Package/). Programs somewhere else (the updater installs the CamClient into its own folder) stay where they are.
		std::error_code error;
		std::filesystem::path folder = std::filesystem::path(executable).parent_path();
		std::filesystem::path project_folder;
		for (int depth = 0; depth < 2 && folder.has_parent_path() && project_folder.empty(); ++depth, folder = folder.parent_path())
		{
			std::filesystem::path bin_folder = folder.parent_path();
			if (bin_folder.filename() == "bin")
			{
				project_folder = bin_folder.parent_path() / project;
			}
		}

		if (project_folder.empty() || !std::filesystem::is_directory(project_folder, error))
		{
			return false;
		}

		std::filesystem::current_path(project_folder, error);
		return !error;
	}
}
