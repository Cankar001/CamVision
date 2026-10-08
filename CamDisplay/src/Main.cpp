#include <algorithm>
#include <csignal>
#include <iostream>

#include "DisplayClient.h"

static DisplayClient *g_Display = nullptr;

// Lets Ctrl+C or a service stop shut down cleanly (the server gets a proper disconnect).
static void OnStopSignal(int)
{
	if (g_Display)
	{
		g_Display->Stop();
	}
}

/// <summary>
/// The display connects to the server and shows the pictures of the cameras. The window always covers the whole screen, without a border. Esc (or Q) shuts the display down.
///
/// Settings come from display.cfg (in the working directory, or --config=path) and can be overridden with --key=value arguments:
///   server_ip, server_port, name, camera, max_fps, window_width, window_height (the last two only for the size of the --save_snapshot picture)
///
/// --save_snapshot=file.jpg is for testing: no window is opened, the first picture with a camera is stored in the file, then the program quits
/// (exit code 0, or 1 if no camera picture was received within --snapshot_timeout=SECONDS, default 30). --snapshot_min_cameras=N waits until N cameras
/// are there, for testing the layout with several cameras.
/// </summary>
// --help: lists everything, which can be given on the command line. Every setting can also be written into display.cfg (without the leading "--").
static void PrintHelp()
{
	std::cout <<
		"CamDisplay - shows the pictures of the cameras full screen. Esc (or Q) shuts it down.\n"
		"\n"
		"Usage: CamDisplay [--help] [--setting=value ...]\n"
		"\n"
		"Settings are read from display.cfg in the working directory (or --config=path). A --setting=value argument overrides the file.\n"
		"\n"
		"Options:\n"
		"  --help                         Shows this text.\n"
		"  --config=path                  Settings file (default: display.cfg).\n"
		"  --server_ip=address            Address of the server.\n"
		"  --server_port=N                Port of the server.\n"
		"  --name=NAME                    Name of this display.\n"
		"  --key=HEX                      Key of this display (made on the server with --add_device=display).\n"
		"  --camera=NAME                  Shows only this camera.\n"
		"  --max_fps=N                    Upper limit of the frames per second.\n"
		"  --window_width=N               --window_height=N     (only for the size of the --save_snapshot picture)\n"
		"\n"
		"Testing:\n"
		"  --save_snapshot=file.jpg       Opens no window, stores the first picture with a camera in the file and quits\n"
		"                                 (exit code 0, or 1 if no picture came in time).\n"
		"  --snapshot_timeout=SECONDS     How long to wait for a picture (default 30).\n"
		"  --snapshot_min_cameras=N       Waits until N cameras are there (to test the layout).\n"
		<< std::endl;
}

int main(int argc, char *argv[])
{
	for (int i = 1; i < argc; ++i)
	{
		std::string argument(argv[i]);
		if (argument == "--help" || argument == "-h" || argument == "-?" || argument == "/?")
		{
			PrintHelp();
			return 0;
		}
	}

	Core::Init();
	Core::Process::EnterProjectFolder("CamDisplay");

	Core::Config settings(argc, argv, "display.cfg");

	DisplayClientConfig config;
	config.ServerIP = settings.GetString("server_ip", config.ServerIP);
	config.Port = (uint16)settings.GetInt("server_port", config.Port);
	config.Name = settings.GetString("name", config.Name);
	config.Key = settings.GetString("key", config.Key);
	config.Camera = settings.GetString("camera", config.Camera);
	config.MaxFPS = (uint32)std::max(settings.GetInt("max_fps", config.MaxFPS), 1);
	config.WindowWidth = (uint32)std::max(settings.GetInt("window_width", config.WindowWidth), 320);
	config.WindowHeight = (uint32)std::max(settings.GetInt("window_height", config.WindowHeight), 240);
	config.SnapshotFile = settings.GetString("save_snapshot", "");
	config.SnapshotTimeoutSeconds = (uint32)std::max(settings.GetInt("snapshot_timeout", config.SnapshotTimeoutSeconds), 1);
	config.SnapshotMinCameras = (uint32)std::max(settings.GetInt("snapshot_min_cameras", config.SnapshotMinCameras), 1);

	if (!settings.LoadedFile().empty())
	{
		CAM_LOG_INFO("Loaded settings from {}", settings.LoadedFile());
	}
	else
	{
		CAM_LOG_INFO("No display.cfg found in the working directory, using defaults and command line arguments.");
	}

	DisplayClient display(config);

	g_Display = &display;
	std::signal(SIGINT, OnStopSignal);
	std::signal(SIGTERM, OnStopSignal);

	int exit_code = display.Run();
	g_Display = nullptr;

	Core::Shutdown();
	return exit_code;
}
