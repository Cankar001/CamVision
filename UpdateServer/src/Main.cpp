#include <algorithm>
#include <iostream>

#include "Server.h"

/// <summary>
/// Settings come from update_server.cfg (in the working directory, or --config=path) and can be overridden with --key=value arguments:
///   port, source_path, binary_path, public_key_path, private_key_path, signature_path, version, regenerate_keys, client_speed_limit_kb
/// </summary>
int main(int argc, char *argv[])
{
	Core::Init();

	// Set the current working directory
	Core::FileSystem::Get()->SetCurrentWorkingDirectory("../../../");

	Core::Config settings(argc, argv, "update_server.cfg");

#ifdef _WIN32
	const char *default_binary_path = "../CamClient/bin/Debug-windows-x86_64/CamClient";
#else
	const char *default_binary_path = "../CamClient/bin/Release-linux/CamClient";
#endif

	ServerConfig config;
	config.ServerPort = (uint16)settings.GetInt("port", config.ServerPort);
	config.Version = (uint32)std::max(settings.GetInt("version", 0), 0);
	config.ClientSpeedLimitKB = (uint32)std::max(settings.GetInt("client_speed_limit_kb", config.ClientSpeedLimitKB), 0);
	config.TargetSourcePath = settings.GetString("source_path", "../CamClient");
	config.TargetBinaryPath = settings.GetString("binary_path", default_binary_path);
	config.PublicKeyPath = settings.GetString("public_key_path", "../CamClient/public_key.key");
	config.PrivateKeyPath = settings.GetString("private_key_path", "../CamClient/private_key.key");
	config.SignaturePath = settings.GetString("signature_path", "../CamClient/signature.sig");

	if (!settings.LoadedFile().empty())
	{
		CAM_LOG_INFO("Loaded settings from {}", settings.LoadedFile());
	}
	else
	{
		CAM_LOG_INFO("No update_server.cfg found in the working directory, using defaults and command line arguments.");
	}

	// The server also starts, if there is no version (yet), for example because the binaries are not there. It waits for them and tells the clients, that they are up to date.
	Server *s = new Server(config);

	// The keys are only regenerated on request, all clients have to trust the new key afterwards.
	// If there are no binaries yet, the server still starts and waits for them (hot reloading), clients are told that they are up to date meanwhile.
	if (!s->LoadUpdateFile(settings.GetBool("regenerate_keys", false)))
	{
		CAM_LOG_WARN("No update available yet. Waiting for binaries in {}.", config.TargetBinaryPath);
	}

	s->StartFileWatcher();

	s->Run();

	delete s;
	Core::Shutdown();
	return 0;
}
