#include <algorithm>
#include <iostream>

#include "Client.h"

/// <summary>
/// The client starts automatically, if the camera is turned on.
/// It first asks the server, if an update is avaiable and updates the binaries, if an update is received.
/// After that it starts the actual camera client. If no update is possible (server not reachable, invalid signature, ...),
/// the camera client, which is already installed, is started anyway.
///
/// Settings come from update_client.cfg (in the working directory, or --config=path) and can be overridden with --key=value arguments:
///   server_ip, server_port, target_path, install_path, fallback_path, public_key_path, camclient_stop_timeout
///
/// A CamClient, which is running, is stopped before an update is installed (its files are locked, and the update client starts the new one afterwards).
/// If no update is installed and a CamClient is running already, no second one is started.
///
/// With --query-version, the client only asks the update server for its version, prints it (just the number, nothing else) and quits, without
/// downloading, installing or starting anything. Exit code 0: the version was printed. Exit code 1: the server did not answer (nothing is printed
/// to stdout, the reason goes to stderr). If the server has no update to offer, the own version is printed, so "printed version differs from my
/// version" tells a program (like the camera client), that a newer update is available (the server answers with the own version, if its update is not newer). --query-timeout=SECONDS changes how long to wait (default 5).
/// </summary>
/// <param name="argc"></param>
/// <param name="argv"></param>
/// <returns></returns>
int main(int argc, char *argv[])
{
	// When stopping a CamClient on Windows, this program is started again as a small helper (see Core::Process::HandleHelperCommand).
	if (Core::Process::HandleHelperCommand(argc, argv))
	{
		return 0;
	}

	// The query mode must print nothing but the version, so it is detected first, before anything is logged or printed.
	// It is a command line flag only (the settings file needs the working directory, which is set below).
	Core::Config arguments(argc, argv, "");
	bool query_version = arguments.GetBool("query_version", false);
	if (query_version)
	{
		Core::Logger::SetSilent(true);
	}

	Core::Init();

	// Set the current working directory
	std::string cwd = "";
	Core::FileSystem::Get()->SetCurrentWorkingDirectory("../../../");
	Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);
	if (!query_version)
	{
		std::cout << "Current CWD: " << cwd.c_str() << std::endl;
	}

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
	config.StopTimeoutSeconds = (uint32)std::max(settings.GetInt("camclient_stop_timeout", 15), 1);

	if (!settings.LoadedFile().empty())
	{
		CAM_LOG_INFO("Loaded settings from {}", settings.LoadedFile());
	}
	else
	{
		CAM_LOG_INFO("No update_client.cfg found in the working directory, using defaults and command line arguments.");
	}

	if (query_version)
	{
		// Nothing is changed in this mode: no leftover is removed, nothing is downloaded, installed or started.
		uint32 timeout_seconds = (uint32)std::max(settings.GetInt("query_timeout", 5), 1);
		Client c(config);

		uint32 version = 0;
		bool answered = c.QueryServerVersion(&version, timeout_seconds * 1000);
		if (answered)
		{
			std::cout << version << std::endl;
		}
		else
		{
			std::cerr << "The update server " << config.ServerIP << ":" << config.Port << " did not answer." << std::endl;
		}

		Core::Shutdown();
		return answered ? 0 : 1;
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
