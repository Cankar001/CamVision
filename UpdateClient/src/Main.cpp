#include <iostream>

#include "Client.h"

/// <summary>
/// The client starts automatically, if the camera is turned on.
/// It first asks the server, if an update is avaiable and updates the binaries, if an update is received.
/// After that it starts the actual camera client. If no update is possible (server not reachable, invalid signature, ...),
/// the camera client, which is already installed, is started anyway.
///
/// Settings come from update_client.cfg (in the working directory, or --config=path) and can be overridden with --key=value arguments:
///   server_ip, server_port, target_path, install_path, fallback_path, public_key_path
/// </summary>
/// <param name="argc"></param>
/// <param name="argv"></param>
/// <returns></returns>
int main(int argc, char *argv[])
{
	Core::Init();

	// Set the current working directory
	std::string cwd = "";
	Core::FileSystem::Get()->SetCurrentWorkingDirectory("../../../");
	Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);
	std::cout << "Current CWD: " << cwd.c_str() << std::endl;

	Core::Config settings(argc, argv, "update_client.cfg");

	// Where the CamClient is, as long as no update was installed (the build output of this machine).
#ifdef _WIN32
	const char *default_fallback_path = "../CamClient/bin/Debug-windows-x86_64/CamClient";
#else
	const char *default_fallback_path = "../CamClient/bin/Release-linux/CamClient";
#endif

	// Create the client
	ClientConfig config;
	config.ServerIP = settings.GetString("server_ip", "127.0.0.1");
	config.Port = (uint16)settings.GetInt("server_port", 44200);
	config.UpdateTargetPath = settings.GetString("target_path", "../CamClient");
	config.UpdateBinaryPath = settings.GetString("install_path", "../CamClient/new_update");
	config.PublicKeyPath = settings.GetString("public_key_path", "../CamClient/public_key.key");
	config.FallbackPath = settings.GetString("fallback_path", default_fallback_path);

	if (!settings.LoadedFile().empty())
	{
		CAM_LOG_INFO("Loaded settings from {}", settings.LoadedFile());
	}
	else
	{
		CAM_LOG_INFO("No update_client.cfg found in the working directory, using defaults and command line arguments.");
	}

	// An update.zip, which is left over from a run that did not finish, must not block the next update.
	std::string leftover_update = config.UpdateBinaryPath + "/update.zip";
	if (Core::FileSystem::Get()->FileExists(leftover_update))
	{
		if (Core::FileSystem::Get()->RemoveFile(leftover_update))
		{
			CAM_LOG_INFO("Removed the leftover update file {}", leftover_update);
		}
		else
		{
			CAM_LOG_ERROR("Could not remove the leftover update file {}", leftover_update);
		}
	}

	Client c(config);

	// Run the client
	c.Run();

	Core::Shutdown();
	return 0;
}
