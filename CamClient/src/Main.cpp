#include <csignal>
#include <iostream>

#include "Client.h"

#if 0
#include "PythonCamera.h"

int main(int argc, char *argv[])
{
	PythonCamera::Camera c(false, 1280, 720);
	c.Stream();
	c.Show();
	c.ReleaseStream();

	return 0;
}
#endif

static Client *g_Client = nullptr;

// Lets Ctrl+C or a service stop shut down cleanly (the server gets a proper disconnect).
static void OnStopSignal(int)
{
	if (g_Client)
	{
		g_Client->Stop();
	}
}

int main(int argc, char *argv[])
{
	Core::Init();

	// Settings come from client.cfg (in the working directory, or --config=path) and can be overridden with --key=value arguments.
	Core::Config settings(argc, argv, "client.cfg");

	ClientConfig config;
	config.ServerIP = settings.GetString("server_ip", config.ServerIP);
	config.Port = (uint16)settings.GetInt("server_port", config.Port);
	config.Name = settings.GetString("name", config.Name);
	config.FPS = (uint32)settings.GetInt("fps", config.FPS);
	config.JpegQuality = settings.GetInt("jpeg_quality", config.JpegQuality);
	config.SendWidth = (uint32)std::max(settings.GetInt("send_width", config.SendWidth), 0);
	config.MaxFPS = (uint32)std::max(settings.GetInt("max_fps", config.MaxFPS), 0);
	config.Camera.Index = settings.GetInt("camera_index", config.Camera.Index);
	config.Camera.Width = (uint32)settings.GetInt("camera_width", config.Camera.Width);
	config.Camera.Height = (uint32)settings.GetInt("camera_height", config.Camera.Height);
	config.Camera.FlipImage = settings.GetBool("flip_image", config.Camera.FlipImage);

	// Headless: no windows, frames are only sent to the server. For devices without a display.
	bool headless = settings.GetBool("headless", false);

	if (!settings.LoadedFile().empty())
	{
		CAM_LOG_INFO("Loaded settings from {}", settings.LoadedFile());
	}
	else
	{
		CAM_LOG_INFO("No client.cfg found in the working directory, using defaults and command line arguments.");
	}

	Client c(config);

	g_Client = &c;
	std::signal(SIGINT, OnStopSignal);
	std::signal(SIGTERM, OnStopSignal);

	// start all worker threads
	c.Run(!headless);
	g_Client = nullptr;

	Core::Shutdown();
	return 0;
}