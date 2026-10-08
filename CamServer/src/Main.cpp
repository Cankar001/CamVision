#include <csignal>
#include <cstdlib>
#include <iostream>

#include "Server.h"

static Server *g_Server = nullptr;

// Ctrl+C (or a service stop) ends the server properly: the recordings, which are being written, are finished and playable.
static void OnStopSignal(int)
{
	if (g_Server)
	{
		g_Server->Stop();
	}
}

// Sends a command to the running server (control port) and prints the answer. Returns the exit code of the program.
static int SendControlCommand(uint16 port, const std::string &command)
{
	if (port == 0)
	{
		std::cerr << "The control port is turned off (control_port = 0)." << std::endl;
		return 1;
	}

	Core::Socket *socket = Core::Socket::Create();
	if (!socket->Open() || !socket->SetNonBlocking(true))
	{
		std::cerr << "Could not open a socket." << std::endl;
		delete socket;
		return 1;
	}

	Core::addr_t server = socket->Lookup("127.0.0.1", port);
	socket->Send(command.c_str(), (int32)command.size(), server);

	// Saving a long video takes a moment.
	static Byte BUF[8192];
	for (int waited_ms = 0; waited_ms < 120000; waited_ms += 20)
	{
		Core::addr_t sender = {};
		int32 len = socket->Recv(BUF, sizeof(BUF) - 1, &sender);
		if (len > 0)
		{
			BUF[len] = 0;
			std::string reply((const char *)BUF);
			std::cout << reply << std::endl;
			delete socket;
			return reply.rfind("ERROR", 0) == 0 ? 1 : 0;
		}

		Core::SleepMS(20);
	}

	std::cerr << "No answer from the server on port " << port << ". Is it running (and is control_port the same)?" << std::endl;
	delete socket;
	return 1;
}

// --add_device=camera|display --name="Front door": makes a key for a new device and stores it. --remove_device="Front door", --list_devices.
static int ManageDevices(const Core::Config &settings, const std::string &file)
{
	Core::DeviceRegistry registry(file);
	std::string error;
	if (!registry.Load(&error))
	{
		std::cerr << "Problems in " << file << ": " << error << std::endl;
	}

	std::string remove = settings.GetString("remove_device", "");
	if (!remove.empty())
	{
		if (!registry.Remove(remove, &error))
		{
			std::cerr << "Could not remove the device: " << error << std::endl;
			return 1;
		}

		std::cout << "Removed the device \"" << remove << "\". Its key does not work anymore (a running server notices that within seconds and ends its connection)." << std::endl;
		return 0;
	}

	std::string add = settings.GetString("add_device", "");
	if (!add.empty())
	{
		Core::DeviceRole role = Core::DeviceRoleFromName(add);
		std::string name = settings.GetString("name", "");
		if (role == Core::DeviceRole::None || name.empty())
		{
			std::cerr << "Usage: CamServer --add_device=camera --name=\"Front door\"      (or --add_device=display --name=\"Living room\")" << std::endl;
			return 1;
		}

		Core::Device device;
		if (!registry.Add(role, name, &device, &error))
		{
			std::cerr << "Could not add the device: " << error << std::endl;
			return 1;
		}

		bool camera = role == Core::DeviceRole::Camera;
		std::cout << "Added the " << Core::DeviceRoleName(role) << " \"" << device.Name << "\" to " << file << "." << std::endl << std::endl;
		std::cout << "Put these lines into " << (camera ? "CamClient/client.cfg" : "CamDisplay/display.cfg") << " of the " << Core::DeviceRoleName(role) << " (on the device itself):" << std::endl << std::endl;
		std::cout << "    key = " << Core::BytesToHex(device.Key, Core::crypto::KEY_BYTES) << std::endl;
		if (camera)
		{
			std::cout << "    name = " << device.Name << std::endl;
		}

		std::cout << std::endl << "The key is also stored in " << file << " on this computer. Keep both secret: whoever has the key can pretend to be this device." << std::endl;
		std::cout << "A running server does not need to be restarted." << std::endl;
		return 0;
	}

	// --list_devices: the names and roles, never the keys.
	std::vector<Core::Device> devices = registry.List();
	if (devices.empty())
	{
		std::cout << "There are no devices in " << file << " yet. Add one with: CamServer --add_device=camera --name=\"Front door\"" << std::endl;
		return 0;
	}

	std::cout << devices.size() << " devices in " << file << ":" << std::endl;
	for (const Core::Device &device : devices)
	{
		std::cout << "  " << Core::DeviceRoleName(device.Role) << "  " << device.Name << "  (key id " << Core::KeyIdToString(device.KeyId) << ")" << std::endl;
	}

	return 0;
}

// --help: lists everything, which can be given on the command line. Every setting can also be written into server.cfg (without the leading "--").
static void PrintHelp()
{
	std::cout <<
		"CamServer - receives the video of the cameras, shows it on the displays, records it and detects faces.\n"
		"\n"
		"Usage: CamServer [start_directory] [--command] [--setting=value ...]\n"
		"\n"
		"Settings are read from server.cfg in the working directory (or --config=path). A --setting=value argument overrides the file.\n"
		"The first argument without leading \"--\" is used as the working directory.\n"
		"\n"
		"Commands (they do their job and quit):\n"
		"  --help                         Shows this text.\n"
		"  --add_device=camera|display    Makes a key for a new device (needs --name=\"Front door\").\n"
		"  --remove_device=NAME           Removes a device, its key does not work anymore.\n"
		"  --list_devices                 Lists the devices (names and roles, never the keys).\n"
		"  --show_websocket_token         Prints the secret of the remote control (it is made, if there is none yet).\n"
		"  --email_test                   Sends a test email with the email settings.\n"
		"  --face_test=photo.jpg          Analyzes one photo and prints the result.\n"
		"\n"
		"Commands for a running server (sent to the control port):\n"
		"  --record_now=MINUTES           Saves the last minutes of video (optional: --record_camera=NAME).\n"
		"  --control_status               Prints the status of the server.\n"
		"  --control_stop                 Stops the server.\n"
		"\n"
		"General settings:\n"
		"  --config=path                  Settings file (default: server.cfg).\n"
		"  --port=N                       Port of the server.\n"
		"  --backup_minutes=N             Length of the video backup in minutes.\n"
		"  --preview=true|false           Shows the frame previews.\n"
		"  --client_timeout_seconds=N     Seconds without a frame until a client counts as gone.\n"
		"  --control_port=N               Port for the commands above (0 = off).\n"
		"  --auth=true|false              Only devices with a key may connect.\n"
		"  --devices_file=path            File with the devices and their keys.\n"
		"\n"
		"Remote control (websocket):\n"
		"  --websocket_port=N             Port (0 = off).\n"
		"  --websocket_bind=address       Address to listen on.\n"
		"  --websocket_token=secret       Secret of the remote control.\n"
		"  --websocket_token_file=path    File with the secret.\n"
		"\n"
		"Faces:\n"
		"  --faces=true|false             Turns the face recognition on or off.\n"
		"  --face_detector_model=path     --face_recognizer_model=path     --known_faces_path=path\n"
		"  --face_score_threshold=F       --face_match_threshold=F         --face_detect_width=N\n"
		"  --face_fps=N                   --face_on_displays=true|false    --face_event_cooldown=SECONDS\n"
		"  --face_snapshots=true|false    --face_snapshot_path=path\n"
		"  --face_on_saved_clips=true|false\n"
		"\n"
		"Recording:\n"
		"  --recordings_path=path         --record_schedule=SCHEDULE       --record_cameras=A,B\n"
		"  --record_segment_minutes=N     --record_keep_days=N             --record_max_gb=N\n"
		"  --record_default_minutes=N     --record_event_minutes=N         --record_on_unknown_person=true|false\n"
		"\n"
		"Email notifications:\n"
		"  --email=true|false             --email_to=a@b.c,d@e.f           --email_from=address\n"
		"  --email_smtp_server=host       --email_smtp_port=N              --email_security=MODE\n"
		"  --email_user=name              --email_password=secret          (or the environment variable CAMVISION_EMAIL_PASSWORD)\n"
		"  --email_subject_prefix=text    --email_attach_snapshot=true|false  --email_batch=true|false\n"
		"  --email_collect_seconds=N      --email_min_interval=SECONDS     --email_max_attachments=N\n"
		"  --email_camera_offline=true|false  --email_verify_certificate=true|false\n"
		"  --email_curl_path=path         --email_timeout=SECONDS\n"
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
	Core::Process::EnterProjectFolder("CamServer");

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
	faces.DrawOnSavedClips = settings.GetBool("face_on_saved_clips", faces.DrawOnSavedClips);
	faces.Snapshots = settings.GetBool("face_snapshots", faces.Snapshots);
	faces.SnapshotPath = settings.GetString("face_snapshot_path", faces.SnapshotPath);
	faces.EventCooldownSeconds = (uint32)std::max(settings.GetInt("face_event_cooldown", faces.EventCooldownSeconds), 0);

	RecorderConfig &recording = config.Recording;
	recording.Path = settings.GetString("recordings_path", recording.Path);
	recording.Schedule = settings.GetString("record_schedule", recording.Schedule);
	recording.SegmentMinutes = (uint32)std::max(settings.GetInt("record_segment_minutes", recording.SegmentMinutes), 1);
	recording.KeepDays = (uint32)std::max(settings.GetInt("record_keep_days", recording.KeepDays), 0);
	recording.MaxGigabytes = (uint32)std::max(settings.GetInt("record_max_gb", recording.MaxGigabytes), 0);
	{
		std::string cameras = settings.GetString("record_cameras", "");
		std::string current;
		for (size_t i = 0; i <= cameras.size(); ++i)
		{
			if (i == cameras.size() || cameras[i] == ',' || cameras[i] == ';')
			{
				size_t begin = current.find_first_not_of(" ");
				size_t end = current.find_last_not_of(" ");
				if (begin != std::string::npos)
				{
					recording.Cameras.push_back(current.substr(begin, end - begin + 1));
				}

				current.clear();
			}
			else
			{
				current += cameras[i];
			}
		}
	}

	config.RecordDefaultMinutes = (uint32)std::max(settings.GetInt("record_default_minutes", config.RecordDefaultMinutes), 1);
	config.RecordOnUnknownPerson = settings.GetBool("record_on_unknown_person", config.RecordOnUnknownPerson);
	config.RecordEventMinutes = (uint32)std::max(settings.GetInt("record_event_minutes", config.RecordEventMinutes), 1);
	config.ControlPort = (uint16)std::max(settings.GetInt("control_port", config.ControlPort), 0);
	config.WebSocketPort = (uint16)std::max(settings.GetInt("websocket_port", config.WebSocketPort), 0);
	config.WebSocketBind = settings.GetString("websocket_bind", config.WebSocketBind);
	config.WebSocketToken = settings.GetString("websocket_token", config.WebSocketToken);
	config.WebSocketTokenFile = settings.GetString("websocket_token_file", config.WebSocketTokenFile);

	// --show_websocket_token: prints the secret of the remote control (it is made, if there is none yet) and quits.
	if (settings.GetBool("show_websocket_token", false))
	{
		std::string token_error;
		std::string token = LoadOrCreateWebSocketToken(config, &token_error);
		if (token.empty())
		{
			std::cerr << "Could not get the token: " << token_error << std::endl;
			Core::Shutdown();
			return 1;
		}

		std::cout << token << std::endl;
		Core::Shutdown();
		return 0;
	}

	config.RequireAuth = settings.GetBool("auth", config.RequireAuth);
	config.DevicesFile = settings.GetString("devices_file", config.DevicesFile);

	// The commands to manage the devices (cameras and displays), which are allowed to connect. They change the file with the devices and quit; a running
	// server notices the change on its own.
	if (!settings.GetString("add_device", "").empty() || !settings.GetString("remove_device", "").empty() || settings.GetBool("list_devices", false))
	{
		int exit_code = ManageDevices(settings, config.DevicesFile);
		Core::Shutdown();
		return exit_code;
	}

	// The commands for a server, which is already running (see the control port). This program just sends the command and prints the answer.
	std::string command;
	if (settings.GetInt("record_now", 0) > 0)
	{
		command = "save " + std::to_string(settings.GetInt("record_now", 0)) + (settings.GetString("record_camera", "").empty() ? "" : " " + settings.GetString("record_camera", ""));
	}
	else if (settings.GetBool("control_status", false))
	{
		command = "status";
	}
	else if (settings.GetBool("control_stop", false))
	{
		command = "stop";
	}

	if (!command.empty())
	{
		int exit_code = SendControlCommand(config.ControlPort, command);
		Core::Shutdown();
		return exit_code;
	}

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
	email.BatchEvents = settings.GetBool("email_batch", email.BatchEvents);
	email.CollectSeconds = (uint32)std::max(settings.GetInt("email_collect_seconds", email.CollectSeconds), 0);
	email.MinIntervalSeconds = (uint32)std::max(settings.GetInt("email_min_interval", email.MinIntervalSeconds), 0);
	email.MaxAttachments = (uint32)std::max(settings.GetInt("email_max_attachments", email.MaxAttachments), 0);
	email.ReportCameraOffline = settings.GetBool("email_camera_offline", email.ReportCameraOffline);
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
	g_Server = &s;
	std::signal(SIGINT, OnStopSignal);
	std::signal(SIGTERM, OnStopSignal);

	s.StartRecording();
	s.StartNotifications();
	s.StartFaceAnalysis();
	s.StartRemoteControl();
	s.Run();

	Core::Shutdown();
	return 0;
}

