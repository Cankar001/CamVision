#include "Server.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <unordered_set>

#include "Messages.h"

#include "Core/Log.h"

// Size of the kernel receive buffer. Large enough to hold a burst of datagrams for several frames of multiple clients.
#define SOCKET_BUFFER_SIZE (8 * 1024 * 1024)

// Upper limit for the frames per second a client may announce, to keep the ring buffer size sane.
#define MAX_CLIENT_FPS 120

Server::Server(const ServerConfig &config)
	: m_Config(config)
{
	m_Socket = Core::Socket::Create();

	std::string cwd = "";
	Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);
	m_Version = Core::utils::GetLocalVersion(cwd);

	CAM_LOG_INFO("===================== CONFIG ===================================");
	CAM_LOG_INFO("IP                    : {}", config.ServerIP);
	CAM_LOG_INFO("Port                  : {}", config.Port);
	CAM_LOG_INFO("Video backup duration : {}", config.VideoBackupDuration);
	CAM_LOG_INFO("Current Server version: {}", m_Version);
	CAM_LOG_INFO("Current CWD           : {}", cwd);
	CAM_LOG_INFO("================================================================");
}

Server::~Server()
{
	if (m_FramePreviewThread.joinable())
	{
		m_FramePreviewThread.join();
	}

	if (m_FaceThread.joinable())
	{
		m_FaceThread.join();
	}

	if (m_ReaperThread.joinable())
	{
		m_ReaperThread.join();
	}

	m_Clients.clear();

	delete m_Socket;
	m_Socket = nullptr;
}

void Server::Run()
{
	m_Running = true;
	CAM_LOG_INFO("Waiting for clients to connect...");

	if (m_Config.ClientTimeoutSeconds != 0)
	{
		m_ReaperThread = std::thread(&Server::ReapStaleClients, std::ref(*this));
	}

	for (;;)
	{
		if (!m_Socket->Open())
		{
			CAM_LOG_ERROR("Could not open socket!");
			break;
		}

		if (!m_Socket->SetBufferSizes(SOCKET_BUFFER_SIZE, SOCKET_BUFFER_SIZE))
		{
			CAM_LOG_ERROR("Could not set socket buffer sizes, frames might get dropped!");
		}

		if (!m_Socket->Bind(m_Config.Port))
		{
			CAM_LOG_ERROR("Could not bind socket");
			break;
		}

		for (;;)
		{
			if (!Step())
			{
				break;
			}
		}

		CAM_LOG_ERROR("Network interface failure.");
		m_Socket->Close();

		Core::SleepMS(10);
	}

	m_Running = false;

	if (m_ReaperThread.joinable())
	{
		m_ReaperThread.join();
	}
}

void Server::StartFramePreviews()
{
	m_FramePreviewThread = std::thread(&Server::FramePreview, std::ref(*this));
}

bool Server::Step()
{
	static Byte BUF[65536];

	Core::addr_t addr;
	int32 len = m_Socket->Recv(BUF, sizeof(BUF), &addr);
	if (len < 0)
	{
		return false;
	}

	if (len < sizeof(header_t))
	{
		return true;
	}

	header_t *header = (header_t *)BUF;
	bool message_success = false;
	switch (header->Type)
	{
		case CLIENT_CONNECTION_START:
			message_success = OnClientConnected(addr, BUF, len);
			break;

		case CLIENT_CONNECTION_CLOSE:
			message_success = OnClientDisconnected(addr, BUF, len);
			break;

		case CLIENT_FRAME:
			message_success = OnClientFrameChunk(addr, BUF, len);
			break;

		case CLIENT_HEARTBEAT:
			message_success = OnClientHeartbeat(addr, BUF, len);
			break;
	}

	if (!message_success)
	{
		CAM_LOG_DEBUG("Ignored invalid message of type {}.", header->Type);
	}

	// A single malformed datagram must not tear down the socket, only socket errors do.
	return true;
}

bool Server::OnClientConnected(Core::addr_t &clientAddr, Byte *message, int32 addrLen)
{
	header_t *header = (header_t *)message;
	if (header->Version != m_Version)
	{
		CAM_LOG_ERROR("Version did not match with server version!");
		return false;
	}

	if (addrLen != sizeof(ClientConnectionStartMessage))
	{
		CAM_LOG_ERROR("Request size was not as expected!");
		return false;
	}

	CAM_LOG_DEBUG("Client {} tries to connect!", clientAddr.Value);
	ClientConnectionStartMessage *msg = (ClientConnectionStartMessage *)message;

	// The name comes from the network, make sure it is terminated.
	char name[MAX_FRAME_NAME_LENGTH];
	memcpy(name, msg->FrameName, MAX_FRAME_NAME_LENGTH);
	name[MAX_FRAME_NAME_LENGTH - 1] = 0;

	{
		std::lock_guard<std::mutex> lock(m_ClientsMutex);
		ClientEntry *existing = FindClient(clientAddr);
		if (existing)
		{
			existing->LastSeen = std::chrono::steady_clock::now();
		}
		else
		{
			// No client registered yet

			// Calculate the buffer size for X minutes
			uint32 fps = std::clamp<uint32>(msg->FPS, 1, MAX_CLIENT_FPS);
			uint32 minutes = m_Config.VideoBackupDuration;
			uint32 seconds = minutes * 60;
			uint32 frames = std::max<uint32>(seconds * fps, 1);
			CAM_LOG_DEBUG("Calculated frame count {0} for {1} minutes with {2} fps.", frames, minutes, fps);

			auto client = std::make_unique<ClientEntry>(frames);
			client->Address = clientAddr;
			client->FrameTitle = name;
			m_Clients.push_back(std::move(client));
		}
	}

	// A repeated request from an already registered client (e.g. our response got lost) is accepted as well.
	ServerConnectionStartResponse response = {};
	response.Header.Type = SERVER_CONNECTION_START;
	response.Header.Version = m_Version;
	response.ConnectionAccepted = true;
	m_Socket->Send(&response, sizeof(response), clientAddr);

	CAM_LOG_INFO("Client {} connected successfully!", clientAddr.Value);
	return true;
}

bool Server::OnClientDisconnected(Core::addr_t &clientAddr, Byte *message, int32 addrLen)
{
	header_t *header = (header_t *)message;
	if (header->Version != m_Version)
	{
		CAM_LOG_ERROR("Version did not match with server version!");
		return false;
	}

	if (addrLen != sizeof(ClientConnectionCloseMessage))
	{
		CAM_LOG_ERROR("Request size was not as expected!");
		return false;
	}

	CAM_LOG_DEBUG("Client {} tries to disconnect!", clientAddr.Value);
	ClientConnectionCloseMessage *msg = (ClientConnectionCloseMessage *)message;

	bool client_removed = false;
	{
		std::lock_guard<std::mutex> lock(m_ClientsMutex);
		auto it = std::find_if(m_Clients.begin(), m_Clients.end(), [&](const auto &c) { return c->Address.Value == clientAddr.Value; });
		if (it != m_Clients.end())
		{
			// Client was found, remove the entry (this releases all of its frames)
			m_Clients.erase(it);
			client_removed = true;
		}
	}

	ServerConnectionCloseResponse response = {};
	response.Header.Type = SERVER_CONNECTION_CLOSE;
	response.Header.Version = m_Version;
	response.ConnectionClosed = client_removed;
	m_Socket->Send(&response, sizeof(response), clientAddr);

	CAM_LOG_INFO("Client {} disconnected successfully!", clientAddr.Value);
	return true;
}

bool Server::OnClientHeartbeat(Core::addr_t &clientAddr, Byte *message, int32 addrLen)
{
	header_t *header = (header_t *)message;
	if (header->Version != m_Version || addrLen != sizeof(ClientHeartbeatMessage))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_ClientsMutex);
	ClientEntry *client = FindClient(clientAddr);
	if (client)
	{
		client->LastSeen = std::chrono::steady_clock::now();
	}
	else
	{
		SendClientUnknown(clientAddr);
	}

	return true;
}

void Server::SendClientUnknown(Core::addr_t &clientAddr)
{
	ServerClientUnknownMessage response = {};
	response.Header.Type = SERVER_CLIENT_UNKNOWN;
	response.Header.Version = m_Version;
	m_Socket->Send(&response, sizeof(response), clientAddr);
}

void Server::ReapStaleClients()
{
	const auto timeout = std::chrono::seconds(m_Config.ClientTimeoutSeconds);
	while (m_Running)
	{
		Core::SleepMS(500);

		std::lock_guard<std::mutex> lock(m_ClientsMutex);
		auto now = std::chrono::steady_clock::now();
		for (auto it = m_Clients.begin(); it != m_Clients.end();)
		{
			if (now - (*it)->LastSeen > timeout)
			{
				CAM_LOG_INFO("Client {0} ({1}) timed out after {2} seconds without a message, removing it.", (*it)->Address.Value, (*it)->FrameTitle, m_Config.ClientTimeoutSeconds);
				it = m_Clients.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

ClientEntry *Server::FindClient(const Core::addr_t &clientAddr)
{
	for (auto &client : m_Clients)
	{
		if (client->Address.Value == clientAddr.Value)
		{
			return client.get();
		}
	}

	return nullptr;
}

bool Server::OnClientFrameChunk(Core::addr_t &clientAddr, Byte *message, int32 addrLen)
{
	if (addrLen < (int32)sizeof(ClientFrameChunkMessage))
	{
		CAM_LOG_ERROR("Frame chunk is too small!");
		return false;
	}

	ClientFrameChunkMessage *chunk = (ClientFrameChunkMessage *)message;
	if (chunk->Header.Version != m_Version)
	{
		CAM_LOG_ERROR("Version did not match with server version!");
		return false;
	}

	// Validate the chunk against its own header, nothing from the network is trusted.
	uint32 payload_size = (uint32)addrLen - sizeof(ClientFrameChunkMessage);
	uint32 frame_size = chunk->FrameSize;
	uint32 chunk_count = chunk->ChunkCount;
	uint32 chunk_index = chunk->ChunkIndex;
	if (frame_size == 0 || frame_size > MAX_ENCODED_FRAME_SIZE ||
		chunk_count != (frame_size + FRAME_CHUNK_PAYLOAD_SIZE - 1) / FRAME_CHUNK_PAYLOAD_SIZE ||
		chunk_index >= chunk_count)
	{
		CAM_LOG_ERROR("Frame chunk header is invalid!");
		return false;
	}

	uint32 offset = chunk_index * FRAME_CHUNK_PAYLOAD_SIZE;
	if (payload_size != std::min(FRAME_CHUNK_PAYLOAD_SIZE, frame_size - offset))
	{
		CAM_LOG_ERROR("Frame chunk payload size is invalid!");
		return false;
	}

	std::lock_guard<std::mutex> lock(m_ClientsMutex);
	ClientEntry *client = FindClient(clientAddr);
	if (!client)
	{
		// Unknown sender (e.g. timed out or the server was restarted). Tell it once per frame, so it reconnects.
		if (chunk_index == 0)
		{
			SendClientUnknown(clientAddr);
		}

		return true;
	}

	client->LastSeen = std::chrono::steady_clock::now();

	FrameAssembly &assembly = client->Assembly;
	if (assembly.Active && chunk->FrameId != assembly.FrameId)
	{
		// Wrap-around safe comparison. Chunks of older frames are late and useless.
		if ((int32)(chunk->FrameId - assembly.FrameId) < 0)
		{
			return true;
		}

		// A newer frame started before the current one was complete, some datagram got lost. Drop the incomplete frame.
		CAM_LOG_DEBUG("Dropped incomplete frame {0} ({1}/{2} chunks).", assembly.FrameId, assembly.ReceivedChunks, assembly.ChunkCount);
		assembly.Active = false;
		++client->DroppedFrames;
	}

	if (!assembly.Active)
	{
		assembly.FrameId = chunk->FrameId;
		assembly.FrameSize = frame_size;
		assembly.ChunkCount = chunk_count;
		assembly.ReceivedChunks = 0;
		assembly.ReceivedMask.assign(chunk_count, false);
		assembly.Data = std::make_shared<std::vector<uchar>>(frame_size);
		assembly.Active = true;
	}
	else if (assembly.FrameSize != frame_size || assembly.ChunkCount != chunk_count)
	{
		CAM_LOG_ERROR("Frame chunk does not match the frame it belongs to!");
		return false;
	}

	if (assembly.ReceivedMask[chunk_index])
	{
		// Duplicate datagram
		return true;
	}

	memcpy(assembly.Data->data() + offset, message + sizeof(ClientFrameChunkMessage), payload_size);
	assembly.ReceivedMask[chunk_index] = true;
	++assembly.ReceivedChunks;

	if (assembly.ReceivedChunks == assembly.ChunkCount)
	{
		// Frame is complete. It is stored still encoded, which keeps the memory footprint of the ring buffer small.
		client->Frames.Push(assembly.Data);
		client->LatestFrame = assembly.Data;
		++client->LatestFrameNumber;

		if (client->LatestFrameNumber % 100 == 0)
		{
			CAM_LOG_INFO("Client {0}: {1} frames received, {2} dropped incomplete.", client->FrameTitle, client->LatestFrameNumber, client->DroppedFrames);
		}

		assembly.Data.reset();
		assembly.Active = false;
	}

	return true;
}

void Server::FramePreview()
{
	struct Preview
	{
		std::string Name;
		EncodedFrame Frame;
		std::vector<FaceResult> Faces;
	};

	std::unordered_map<uint64, uint32> shown_frames;
	std::unordered_set<std::string> created_windows;

	// Windows, which the user closed. They stay closed until the client disconnects and comes back.
	std::unordered_set<std::string> dismissed_windows;

	// OpenCV throws if a window does not exist (anymore), e.g. because the user closed it. That must never take down the server.
	auto destroy_window = [](const std::string &name)
	{
		try
		{
			cv::destroyWindow(name);
		}
		catch (const cv::Exception &)
		{
		}
	};

	auto is_window_visible = [](const std::string &name)
	{
		try
		{
			return cv::getWindowProperty(name, cv::WND_PROP_VISIBLE) >= 1;
		}
		catch (const cv::Exception &)
		{
			return false;
		}
	};

	while (m_Running)
	{
		try
		{
			// Only hold the lock while copying the (shared) frame pointers, decoding and drawing happens without it.
			std::vector<Preview> previews;
			std::unordered_set<std::string> active_names;
			{
				std::lock_guard<std::mutex> lock(m_ClientsMutex);
				for (auto &client : m_Clients)
				{
					active_names.insert(client->FrameTitle);
					uint32 &shown = shown_frames[client->Address.Value];
					if (client->LatestFrame && client->LatestFrameNumber != shown)
					{
						shown = client->LatestFrameNumber;
						previews.push_back({ client->FrameTitle, client->LatestFrame, client->Faces });
					}
				}
			}

			// Detect windows closed by the user (X button).
			for (auto it = created_windows.begin(); it != created_windows.end();)
			{
				if (!is_window_visible(*it))
				{
					dismissed_windows.insert(*it);
					destroy_window(*it);
					it = created_windows.erase(it);
				}
				else
				{
					++it;
				}
			}

			for (Preview &preview : previews)
			{
				if (dismissed_windows.count(preview.Name))
				{
					continue;
				}

				cv::Mat frame = cv::imdecode(*preview.Frame, cv::IMREAD_COLOR);
				if (frame.empty())
				{
					CAM_LOG_ERROR("Could not decode frame of {}!", preview.Name);
					continue;
				}

				if (!preview.Faces.empty())
				{
					// The faces come from the face analysis, which runs a few times per second, so they can be a little behind the frame.
					DrawFaces(frame, preview.Faces, m_FaceAnalyzer && m_FaceAnalyzer->CanRecognize());
				}

				if (created_windows.insert(preview.Name).second)
				{
					// Resizable window, which keeps the aspect ratio of the frame instead of stretching it. Starts with the size of the frame.
					cv::namedWindow(preview.Name, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
					cv::resizeWindow(preview.Name, frame.cols, frame.rows);
				}

				cv::imshow(preview.Name, frame);
			}

			// Close the windows of clients, which disconnected or timed out. A client, which comes back, gets its window again.
			for (auto it = created_windows.begin(); it != created_windows.end();)
			{
				if (active_names.find(*it) == active_names.end())
				{
					destroy_window(*it);
					it = created_windows.erase(it);
				}
				else
				{
					++it;
				}
			}

			for (auto it = dismissed_windows.begin(); it != dismissed_windows.end();)
			{
				it = active_names.find(*it) == active_names.end() ? dismissed_windows.erase(it) : std::next(it);
			}

			char key = (char)cv::waitKey(1);
			if (key == 'q')
			{
				// Close all windows and keep them closed, like closing every window by hand.
				for (const std::string &name : created_windows)
				{
					dismissed_windows.insert(name);
					destroy_window(name);
				}

				created_windows.clear();
			}
		}
		catch (const cv::Exception &e)
		{
			CAM_LOG_ERROR("Preview error: {}", e.what());
			Core::SleepMS(100);
		}
	}
}

void Server::StartFaceAnalysis()
{
	if (!m_Config.Faces.Enabled)
	{
		return;
	}

	auto analyzer = std::make_unique<FaceAnalyzer>(m_Config.Faces);
	if (!analyzer->Initialize())
	{
		CAM_LOG_ERROR("The face analysis is turned off.");
		return;
	}

	m_FaceAnalyzer = std::move(analyzer);
	CAM_LOG_INFO("Face analysis started: {0} per second per camera, {1}.", m_Config.Faces.FPS,
		m_FaceAnalyzer->CanRecognize() ? std::to_string(m_FaceAnalyzer->GetKnownPeopleCount()) + " known people" : std::string("detection only"));

	m_FaceThread = std::thread(&Server::FaceLoop, std::ref(*this));
}

// A name, which can be used as a folder or file name.
static std::string SafeName(const std::string &name)
{
	std::string result = name;
	for (char &c : result)
	{
		if (!isalnum((unsigned char)c) && c != '-' && c != '_')
		{
			c = '_';
		}
	}

	return result.empty() ? "unnamed" : result;
}

static std::string TimestampString()
{
	std::time_t now = std::time(nullptr);
	std::tm local = {};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	char buffer[32];
	strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &local);
	return buffer;
}

void Server::FaceLoop()
{
	struct Job
	{
		uint64 Address;
		std::string Title;
		EncodedFrame Frame;
		uint32 Number;
	};

	const FaceConfig &config = m_Config.Faces;
	const int64 interval_ms = 1000 / std::max<uint32>(config.FPS, 1);
	const int64 cooldown_ms = (int64)config.EventCooldownSeconds * 1000;

	// per camera
	std::unordered_map<uint64, int64> last_analysis_ms;
	std::unordered_map<uint64, uint32> last_number;
	std::unordered_map<uint64, std::unordered_map<std::string, int64>> last_event_ms;

	while (m_Running)
	{
		m_FaceAnalyzer->ReloadKnownFacesIfChanged();

		// Only hold the lock while collecting the (shared) frame pointers, the analysis takes long.
		std::vector<Job> jobs;
		int64 now = Core::QueryMS();
		{
			std::lock_guard<std::mutex> lock(m_ClientsMutex);
			for (auto &client : m_Clients)
			{
				uint64 address = client->Address.Value;
				if (client->LatestFrame && client->LatestFrameNumber != last_number[address] && now - last_analysis_ms[address] >= interval_ms)
				{
					jobs.push_back({ address, client->FrameTitle, client->LatestFrame, client->LatestFrameNumber });
				}
			}
		}

		if (jobs.empty())
		{
			Core::SleepMS(20);
			continue;
		}

		for (Job &job : jobs)
		{
			cv::Mat frame = cv::imdecode(*job.Frame, cv::IMREAD_COLOR);
			last_number[job.Address] = job.Number;
			if (frame.empty())
			{
				continue;
			}

			std::vector<FaceResult> faces;
			try
			{
				faces = m_FaceAnalyzer->Analyze(frame);
			}
			catch (const cv::Exception &e)
			{
				CAM_LOG_ERROR("Face analysis failed for {0}: {1}", job.Title, e.what());
			}

			last_analysis_ms[job.Address] = Core::QueryMS();

			{
				std::lock_guard<std::mutex> lock(m_ClientsMutex);
				for (auto &client : m_Clients)
				{
					if (client->Address.Value == job.Address)
					{
						client->Faces = faces;
						break;
					}
				}
			}

			// Report everybody, who was not reported lately.
			bool recognition = m_FaceAnalyzer->CanRecognize();
			bool snapshot_needed = false;
			std::string snapshot_identity;
			for (const FaceResult &face : faces)
			{
				std::string identity = face.Recognized ? face.Name : (recognition ? "unknown" : "face");
				int64 &last = last_event_ms[job.Address][identity];
				int64 event_time = Core::QueryMS();
				if (last != 0 && event_time - last < cooldown_ms)
				{
					continue;
				}

				last = event_time;
				if (face.Recognized)
				{
					CAM_LOG_INFO("{0}: {1} recognized (similarity {2:.2f})", job.Title, face.Name, face.Similarity);
				}
				else if (recognition)
				{
					CAM_LOG_INFO("{0}: unknown person (best similarity {1:.2f})", job.Title, face.Similarity);
				}
				else
				{
					CAM_LOG_INFO("{0}: face detected (score {1:.2f})", job.Title, face.Score);
				}

				if (!snapshot_needed)
				{
					snapshot_needed = true;
					snapshot_identity = identity;
				}
			}

			if (snapshot_needed && config.Snapshots)
			{
				try
				{
					std::filesystem::path folder = std::filesystem::path(config.SnapshotPath) / SafeName(job.Title);
					std::filesystem::create_directories(folder);

					cv::Mat marked = frame.clone();
					DrawFaces(marked, faces, recognition);
					std::string file = (folder / (TimestampString() + "_" + SafeName(snapshot_identity) + ".jpg")).string();
					if (cv::imwrite(file, marked))
					{
						CAM_LOG_INFO("Snapshot stored: {}", file);
					}
					else
					{
						CAM_LOG_ERROR("Could not store the snapshot {}", file);
					}
				}
				catch (const std::exception &e)
				{
					CAM_LOG_ERROR("Could not store the snapshot: {}", e.what());
				}
			}
		}
	}
}
