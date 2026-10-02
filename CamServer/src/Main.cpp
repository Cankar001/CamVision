#include <iostream>

#include "Server.h"

int main(int argc, char *argv[])
{
	Core::Init();
	Core::FileSystem::Get()->SetCurrentWorkingDirectory("../../../");

	std::string cwd = "";
	bool cwd_success = Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);
	if (cwd_success)
	{
		std::cout << "Current CWD: " << cwd.c_str() << std::endl;
	}

	// Get current start directory from command line arguments
	// First argument without leading "--" will be the start directory (arguments with "--" are settings, see below)
	int cwd_arg = 0;
	for (int i = 1; i < argc; ++i)
	{
		if (std::string(argv[i]).rfind("--", 0) != 0)
		{
			cwd_arg = i;
			break;
		}
	}

	if (cwd_arg != 0)
	{
		std::string selected_cwd = std::string(argv[cwd_arg]);
		cwd_success = Core::FileSystem::Get()->SetCurrentWorkingDirectory(selected_cwd);
		if (cwd_success)
		{
			cwd_success = Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);
			if (cwd_success)
			{
				std::cout << "Changed CWD to: " << cwd.c_str() << std::endl;	
			}
		}
	}

	// Settings come from server.cfg (in the working directory, or --config=path) and can be overridden with --key=value arguments.
	Core::Config settings(argc, argv, "server.cfg");

	ServerConfig config;
	config.Port = (uint16)settings.GetInt("port", config.Port);
	config.VideoBackupDuration = (uint32)std::max(settings.GetInt("backup_minutes", config.VideoBackupDuration), 1);
	config.ShowPreview = settings.GetBool("preview", config.ShowPreview);
	config.ClientTimeoutSeconds = (uint32)std::max(settings.GetInt("client_timeout_seconds", config.ClientTimeoutSeconds), 0);

	FaceConfig &faces = config.Faces;
	faces.Enabled = settings.GetBool("faces", faces.Enabled);
	faces.DetectorModel = settings.GetString("face_detector_model", faces.DetectorModel);
	faces.RecognizerModel = settings.GetString("face_recognizer_model", faces.RecognizerModel);
	faces.KnownFacesPath = settings.GetString("known_faces_path", faces.KnownFacesPath);
	faces.ScoreThreshold = settings.GetFloat("face_score_threshold", faces.ScoreThreshold);
	faces.MatchThreshold = settings.GetFloat("face_match_threshold", faces.MatchThreshold);
	faces.DetectWidth = std::max(settings.GetInt("face_detect_width", faces.DetectWidth), 0);
	faces.FPS = (uint32)std::max(settings.GetInt("face_fps", faces.FPS), 1);
	faces.DrawOnDisplays = settings.GetBool("face_on_displays", faces.DrawOnDisplays);
	faces.Snapshots = settings.GetBool("face_snapshots", faces.Snapshots);
	faces.SnapshotPath = settings.GetString("face_snapshot_path", faces.SnapshotPath);
	faces.EventCooldownSeconds = (uint32)std::max(settings.GetInt("face_event_cooldown", faces.EventCooldownSeconds), 0);

	// --face_test=photo.jpg: analyzes one photo, prints the result and quits. To check the models and the known faces without a camera.
	std::string face_test = settings.GetString("face_test", "");
	if (!face_test.empty())
	{
		int exit_code = RunFaceTest(faces, face_test);
		Core::Shutdown();
		return exit_code;
	}

	if (!settings.LoadedFile().empty())
	{
		CAM_LOG_INFO("Loaded settings from {}", settings.LoadedFile());
	}
	else
	{
		CAM_LOG_INFO("No server.cfg found in the working directory, using defaults and command line arguments.");
	}

	Server s(config);
	if (config.ShowPreview)
	{
		s.StartFramePreviews();
	}
	s.StartFaceAnalysis();
	s.Run();

	Core::Shutdown();
	return 0;
}

