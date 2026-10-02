#include <cstdlib>
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

	EmailConfig &email = config.Email;
	email.Enabled = settings.GetBool("email", email.Enabled);
	email.Server = settings.GetString("email_smtp_server", email.Server);
	email.Port = (uint16)settings.GetInt("email_smtp_port", email.Port);
	email.Security = settings.GetString("email_security", email.Security);
	email.User = settings.GetString("email_user", email.User);
	email.Password = settings.GetString("email_password", email.Password);
	email.From = settings.GetString("email_from", email.From);
	email.SubjectPrefix = settings.GetString("email_subject_prefix", email.SubjectPrefix);
	email.AttachSnapshot = settings.GetBool("email_attach_snapshot", email.AttachSnapshot);
	email.MinIntervalSeconds = (uint32)std::max(settings.GetInt("email_min_interval", email.MinIntervalSeconds), 0);
	email.VerifyCertificate = settings.GetBool("email_verify_certificate", email.VerifyCertificate);
	email.CurlPath = settings.GetString("email_curl_path", email.CurlPath);
	email.TimeoutSeconds = (uint32)std::max(settings.GetInt("email_timeout", email.TimeoutSeconds), 5);

	// The recipients: one or more addresses, separated by commas or semicolons.
	std::string recipients = settings.GetString("email_to", "");
	{
		std::string current;
		for (size_t i = 0; i <= recipients.size(); ++i)
		{
			if (i == recipients.size() || recipients[i] == ',' || recipients[i] == ';')
			{
				size_t begin = current.find_first_not_of(" ");
				size_t end = current.find_last_not_of(" ");
				if (begin != std::string::npos)
				{
					email.To.push_back(current.substr(begin, end - begin + 1));
				}

				current.clear();
			}
			else
			{
				current += recipients[i];
			}
		}
	}

	// A password in a file is readable by everybody, who can read the file. The environment variable is the safer place.
#ifdef _WIN32
	char *environment_password = nullptr;
	size_t environment_password_length = 0;
	if (_dupenv_s(&environment_password, &environment_password_length, "CAMVISION_EMAIL_PASSWORD") == 0 && environment_password)
	{
		if (environment_password[0] != 0)
		{
			email.Password = environment_password;
		}

		free(environment_password);
	}
#else
	if (const char *environment_password = std::getenv("CAMVISION_EMAIL_PASSWORD"))
	{
		if (environment_password[0] != 0)
		{
			email.Password = environment_password;
		}
	}
#endif

	// --email_test: sends a test email with the settings above and quits, to check them.
	if (settings.GetBool("email_test", false))
	{
		Mailer mailer(email);
		if (!mailer.Validate())
		{
			Core::Shutdown();
			return 1;
		}

		EmailMessage message;
		message.Subject = "Test email";
		message.Body = "This is a test email from CamVision.\n\nIf you read this, the email settings in server.cfg are correct.\n";
		std::string error;
		std::cout << "Sending a test email to " << recipients << " via " << email.Server << ":" << email.Port << " ..." << std::endl;
		bool sent = mailer.SendNow(message, &error);
		if (sent)
		{
			std::cout << "The test email was sent." << std::endl;
		}
		else
		{
			std::cerr << "The test email could not be sent: " << error << std::endl;
		}

		Core::Shutdown();
		return sent ? 0 : 1;
	}

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

