#include "Server.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
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
	// The protocol version is independent of the version of the software (CAM_VERSION).
	m_Version = CAM_PROTOCOL_VERSION;
	uint32 software_version = Core::utils::GetLocalVersion(cwd);

	CAM_LOG_INFO("===================== CONFIG ===================================");
	CAM_LOG_INFO("IP                    : {}", config.ServerIP);
	CAM_LOG_INFO("Port                  : {}", config.Port);
	CAM_LOG_INFO("Video backup duration : {}", config.VideoBackupDuration);
	CAM_LOG_INFO("Current Server version: {0} (protocol {1})", software_version, m_Version);
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

	if (m_ForwardThread.joinable())
	{
		m_ForwardThread.join();
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

	m_ForwardThread = std::thread(&Server::ForwardLoop, std::ref(*this));

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

	if (m_ForwardThread.joinable())
	{
		m_ForwardThread.join();
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

		case DISPLAY_CONNECTION_START:
			message_success = OnDisplayConnected(addr, BUF, len);
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
			client->CameraId = m_NextCameraId++;
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

		// A display disconnects with the same message.
		auto display = std::find_if(m_Displays.begin(), m_Displays.end(), [&](const auto &d) { return d->Address.Value == clientAddr.Value; });
		if (display != m_Displays.end())
		{
			CAM_LOG_INFO("Display {0} ({1}) disconnected.", clientAddr.Value, (*display)->Name);
			m_Displays.erase(display);
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
		return true;
	}

	for (auto &display : m_Displays)
	{
		if (display->Address.Value == clientAddr.Value)
		{
			display->LastSeen = std::chrono::steady_clock::now();
			return true;
		}
	}

	SendClientUnknown(clientAddr);
	return true;
}

static bool EqualsIgnoreCase(const std::string &a, const std::string &b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

bool Server::OnDisplayConnected(Core::addr_t &displayAddr, Byte *message, int32 addrLen)
{
	DisplayConnectionStartMessage *msg = (DisplayConnectionStartMessage *)message;
	if (msg->Header.Version != m_Version)
	{
		CAM_LOG_ERROR("Display {}: the protocol version does not match with the server!", displayAddr.Value);
		return false;
	}

	if (addrLen != sizeof(DisplayConnectionStartMessage))
	{
		CAM_LOG_ERROR("Request size was not as expected!");
		return false;
	}

	// The names come from the network, make sure they are terminated.
	char name[MAX_FRAME_NAME_LENGTH];
	char filter[MAX_FRAME_NAME_LENGTH];
	memcpy(name, msg->DisplayName, MAX_FRAME_NAME_LENGTH);
	memcpy(filter, msg->CameraFilter, MAX_FRAME_NAME_LENGTH);
	name[MAX_FRAME_NAME_LENGTH - 1] = 0;
	filter[MAX_FRAME_NAME_LENGTH - 1] = 0;

	// Nobody needs more than a video frame rate, and the server decides if the display has no wish.
	uint32 max_fps = msg->MaxFPS == 0 ? 10 : std::min<uint32>(msg->MaxFPS, 60);

	{
		std::lock_guard<std::mutex> lock(m_ClientsMutex);

		DisplayEntry *display = nullptr;
		for (auto &existing : m_Displays)
		{
			if (existing->Address.Value == displayAddr.Value)
			{
				display = existing.get();
				break;
			}
		}

		if (!display)
		{
			m_Displays.push_back(std::make_unique<DisplayEntry>());
			display = m_Displays.back().get();
			display->Address = displayAddr;
			CAM_LOG_INFO("Display {0} ({1}) connected, showing {2}, up to {3} frames per second per camera.", displayAddr.Value, name, filter[0] ? std::string(filter) : std::string("all cameras"), max_fps);
		}

		// A repeated request (the response got lost, or the display changed its wishes) is accepted as well.
		display->Name = name;
		display->CameraFilter = filter;
		display->MaxFPS = max_fps;
		display->LastSeen = std::chrono::steady_clock::now();
	}

	ServerDisplayStartResponse response = {};
	response.Header.Type = SERVER_DISPLAY_START;
	response.Header.Version = m_Version;
	response.Accepted = true;
	m_Socket->Send(&response, sizeof(response), displayAddr);
	return true;
}

void Server::SendFrameToDisplay(const Core::addr_t &display, uint32 cameraId, const std::string &cameraName, uint32 frameNumber, const EncodedFrame &frame)
{
	if (!frame || frame->empty())
	{
		return;
	}

	uint32 frame_size = (uint32)frame->size();
	uint32 chunk_count = (frame_size + FRAME_CHUNK_PAYLOAD_SIZE - 1) / FRAME_CHUNK_PAYLOAD_SIZE;
	if (chunk_count == 0 || chunk_count > 0xFFFF)
	{
		return;
	}

	Byte packet[sizeof(ServerFrameChunkMessage) + FRAME_CHUNK_PAYLOAD_SIZE];
	ServerFrameChunkMessage *chunk = (ServerFrameChunkMessage *)packet;
	memset(chunk, 0, sizeof(ServerFrameChunkMessage));
	chunk->Header.Type = SERVER_FRAME_CHUNK;
	chunk->Header.Version = m_Version;
	chunk->CameraId = cameraId;
	memcpy(chunk->CameraName, cameraName.c_str(), std::min<size_t>(cameraName.size(), MAX_FRAME_NAME_LENGTH - 1));
	chunk->FrameId = frameNumber;
	chunk->FrameSize = frame_size;
	chunk->ChunkCount = (uint16)chunk_count;

	for (uint32 i = 0; i < chunk_count; ++i)
	{
		uint32 offset = i * FRAME_CHUNK_PAYLOAD_SIZE;
		uint32 payload_size = std::min(FRAME_CHUNK_PAYLOAD_SIZE, frame_size - offset);
		chunk->ChunkIndex = (uint16)i;
		memcpy(packet + sizeof(ServerFrameChunkMessage), frame->data() + offset, payload_size);

		uint32 packet_size = sizeof(ServerFrameChunkMessage) + payload_size;
		if (m_Socket->Send(packet, packet_size, display) != (int32)packet_size)
		{
			// Frames are never retransmitted, the display drops the incomplete frame as soon as a newer one arrives.
			return;
		}
	}
}

void Server::ForwardLoop()
{
	// The faces of the latest analysis are drawn for this long. Later the analysis is considered to be stalled, and the pictures are sent without faces.
	const int64 faces_valid_ms = 2000;

	struct Job
	{
		Core::addr_t Display;
		uint32 CameraId;
		std::string CameraName;
		uint32 FrameNumber;
		EncodedFrame Frame;
		std::vector<FaceResult> Faces;
		bool Recognition;
	};

	while (m_Running)
	{
		// Only hold the lock while collecting what has to be sent, rendering and sending take long.
		std::vector<Job> jobs;
		int64 now = Core::QueryMS();
		bool recognition = m_FaceAnalyzer && m_FaceAnalyzer->CanRecognize();
		bool draw_faces = m_FaceAnalyzer && m_Config.Faces.DrawOnDisplays;
		{
			std::lock_guard<std::mutex> lock(m_ClientsMutex);
			for (auto &display : m_Displays)
			{
				// A display on a weak device or on Wi-Fi asks for fewer frames.
				int64 interval_ms = 1000 / std::max<uint32>(display->MaxFPS, 1);

				for (auto &client : m_Clients)
				{
					if (!client->LatestFrame || client->LatestFrameNumber == 0)
					{
						continue;
					}

					// The display sees one camera, or all of them.
					if (!display->CameraFilter.empty() && !EqualsIgnoreCase(display->CameraFilter, client->FrameTitle))
					{
						continue;
					}

					// Every frame is sent only once to a display, and not more often than the display wants.
					uint32 &last_number = display->LastSentNumber[client->CameraId];
					int64 &last_sent = display->LastSentMS[client->CameraId];
					if (last_number == client->LatestFrameNumber || (last_sent != 0 && now - last_sent < interval_ms))
					{
						continue;
					}

					last_number = client->LatestFrameNumber;
					last_sent = now;

					Job job;
					job.Display = display->Address;
					job.CameraId = client->CameraId;
					job.CameraName = client->FrameTitle;
					job.FrameNumber = client->LatestFrameNumber;
					job.Frame = client->LatestFrame;
					job.Recognition = recognition;
					if (draw_faces && !client->Faces.empty() && now - client->FacesUpdatedMS <= faces_valid_ms)
					{
						job.Faces = client->Faces;
					}

					jobs.push_back(std::move(job));
				}
			}
		}

		if (jobs.empty())
		{
			Core::SleepMS(5);
			continue;
		}

		// A frame, which goes to several displays, is rendered only once.
		std::map<std::pair<uint32, uint32>, EncodedFrame> rendered;
		for (const Job &job : jobs)
		{
			EncodedFrame frame = job.Frame;
			if (!job.Faces.empty())
			{
				auto key = std::make_pair(job.CameraId, job.FrameNumber);
				auto it = rendered.find(key);
				if (it == rendered.end())
				{
					EncodedFrame with_faces;
					try
					{
						cv::Mat image = cv::imdecode(*job.Frame, cv::IMREAD_COLOR);
						if (!image.empty())
						{
							DrawFaces(image, job.Faces, job.Recognition);
							auto encoded = std::make_shared<std::vector<uchar>>();
							if (cv::imencode(".jpg", image, *encoded, { cv::IMWRITE_JPEG_QUALITY, 80 }))
							{
								with_faces = encoded;
							}
						}
					}
					catch (const cv::Exception &e)
					{
						CAM_LOG_ERROR("Could not draw the faces into the picture for the displays: {}", e.what());
					}

					// If it did not work, the plain picture is sent.
					it = rendered.emplace(key, with_faces ? with_faces : job.Frame).first;
				}

				frame = it->second;
			}

			SendFrameToDisplay(job.Display, job.CameraId, job.CameraName, job.FrameNumber, frame);
		}
	}
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

		for (auto it = m_Displays.begin(); it != m_Displays.end();)
		{
			if (now - (*it)->LastSeen > timeout)
			{
				CAM_LOG_INFO("Display {0} ({1}) timed out after {2} seconds without a message, removing it.", (*it)->Address.Value, (*it)->Name, m_Config.ClientTimeoutSeconds);
				it = m_Displays.erase(it);
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

// For messages: 2026-10-02 22:31:07
static std::string ReadableTimeString()
{
	std::time_t now = std::time(nullptr);
	std::tm local = {};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	char buffer[32];
	strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
	return buffer;
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
						client->FacesUpdatedMS = Core::QueryMS();
						break;
					}
				}
			}

			// Report everybody, who was not reported lately.
			bool recognition = m_FaceAnalyzer->CanRecognize();
			bool snapshot_needed = false;
			std::string snapshot_identity;
			bool unknown_person = false;
			FaceResult unknown_face;
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

					// Only with recognition, a face can be "unknown". Without it, every face is just a face.
					if (!unknown_person)
					{
						unknown_person = true;
						unknown_face = face;
					}
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

			std::string snapshot_file;
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
						snapshot_file = file;
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

			if (unknown_person)
			{
				NotifyUnknownPerson(job.Title, unknown_face, snapshot_file);
			}
		}
	}
}

void Server::NotifyUnknownPerson(const std::string &camera, const FaceResult &face, const std::string &snapshotFile)
{
	// TODO: Send this as an email to the owner. For now, only the email, which would be sent, is written to the log.
	//       Still to do for the real email: SMTP settings (server, port, user, password, sender, recipients) in server.cfg, sending without blocking the
	//       face analysis, the snapshot as an attachment, and a limit for the number of emails, if somebody walks around in front of several cameras.
	CAM_LOG_WARN("[TODO email] Would send an email to the owner. Subject: \"Unknown person at {0}\". Text: An unknown person was seen by the camera {0} at {1}. "
		"Best similarity to a known person: {2:.2f} (needed {3:.2f}), detector score {4:.2f}. Snapshot: {5}",
		camera, ReadableTimeString(), face.Similarity, m_Config.Faces.MatchThreshold, face.Score,
		snapshotFile.empty() ? std::string("none (face_snapshots is off)") : snapshotFile);
}
