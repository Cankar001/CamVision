#include <atomic>
#include <csignal>
#include <iostream>

#include "Features.h"
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

static std::atomic<Client *> g_Client{ nullptr };

// A request to stop, which came before the client existed (while it opens the camera, for example). It is carried out as soon as the client is there.
static std::atomic<bool> g_StopRequested{ false };

// Lets Ctrl+C or a service stop shut down cleanly (the server gets a proper disconnect).
static void OnStopSignal(int)
{
	g_StopRequested = true;

	Client *client = g_Client;
	if (client)
	{
		client->Stop();
	}
}

int main(int argc, char *argv[])
{
	Core::Init();

	// Started from the build folder, the working directory is the CamClient folder (client.cfg). Installed by the updater, it stays in its own folder.
	Core::Process::EnterProjectFolder("CamClient");

	// Settings come from client.cfg (in the working directory, or --config=path) and can be overridden with --key=value arguments.
	Core::Config settings(argc, argv, "client.cfg");

	// Ctrl+C and SIGTERM shut the client down cleanly. The update client also stops the CamClient before an update is installed, with a stop request, which
	// on Windows works without a console too (a service, a program without a window), where Ctrl+C does not exist. This is set up first, so a CamClient,
	// which is still starting, can be stopped too.
	std::signal(SIGINT, OnStopSignal);
	std::signal(SIGTERM, OnStopSignal);
	Core::Process::StartStopListener([] { OnStopSignal(0); });

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

#if FRAME_ANALYSIS // FRAME ANALYSIS (movement and face detection in the camera client): switch all blocks with this tag to "#if 1" to enable it
	FrameAnalysisConfig &analysis = config.Analysis;
	analysis.Enabled = settings.GetBool("analysis", analysis.Enabled);
	analysis.DetectMotion = settings.GetBool("analysis_motion", analysis.DetectMotion);
	analysis.MotionMinArea = settings.GetFloat("analysis_motion_min_area", analysis.MotionMinArea);
	analysis.MotionPixelThreshold = std::max(settings.GetInt("analysis_motion_pixel_threshold", analysis.MotionPixelThreshold), 1);
	analysis.DetectFaces = settings.GetBool("analysis_faces", analysis.DetectFaces);
	analysis.FaceModel = settings.GetString("analysis_face_model", analysis.FaceModel);
	analysis.FaceEveryNFrames = (uint32)std::max(settings.GetInt("analysis_face_every", analysis.FaceEveryNFrames), 1);
	analysis.HoldSeconds = (uint32)std::max(settings.GetInt("analysis_hold_seconds", analysis.HoldSeconds), 0);
	analysis.IdleFPS = (uint32)std::max(settings.GetInt("analysis_idle_fps", analysis.IdleFPS), 1);
#endif // FRAME_ANALYSIS

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
	if (g_StopRequested)
	{
		c.Stop();
	}

	// start all worker threads
	c.Run(!headless);
	g_Client = nullptr;

	Core::Shutdown();
	return 0;
}