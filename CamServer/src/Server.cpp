#include "Server.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
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

	if (m_Config.RequireAuth)
	{
		// Everything goes through the secure socket from now on: only devices with a key are heard, and all messages are encrypted.
		m_Devices = std::make_unique<Core::DeviceRegistry>(m_Config.DevicesFile);
		std::string devices_error;
		if (!m_Devices->Load(&devices_error))
		{
			CAM_LOG_ERROR("Problems in the list of devices: {}", devices_error);
		}

		m_Secure = Core::SecureSocket::CreateServer(m_Socket, m_Devices.get());
		m_Socket = m_Secure;
	}

	std::string cwd = "";
	Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);
	// The protocol version is independent of the version of the software (CAM_VERSION).
	m_Version = CAM_PROTOCOL_VERSION;
	uint32 software_version = Core::utils::GetLocalVersion(cwd);

	CAM_LOG_INFO("===================== CONFIG ===================================");
	CAM_LOG_INFO("IP                    : {}", config.ServerIP);
	CAM_LOG_INFO("Port                  : {}", config.Port);
	CAM_LOG_INFO("Video backup duration : {}", config.VideoBackupDuration);
	if (m_Devices)
	{
		CAM_LOG_INFO("Device keys           : required, {0} devices in {1}", m_Devices->Count(), config.DevicesFile);
	}
	else
	{
		CAM_LOG_INFO("Device keys           : OFF");
	}
	CAM_LOG_INFO("Current Server version: {0} (protocol {1})", software_version, m_Version);
	CAM_LOG_INFO("Current CWD           : {}", cwd);
	CAM_LOG_INFO("================================================================");

	if (m_Devices && m_Devices->Count() == 0)
	{
		CAM_LOG_WARN("There are no devices in {} yet, so no camera and no display can connect. Add them with: CamServer --add_device=camera --name=\"Front door\"", config.DevicesFile);
	}

	if (!m_Devices)
	{
		CAM_LOG_WARN("The authentication is OFF (auth = false): everybody in the network can connect as a camera or as a display, and the pictures are not encrypted. Only use this in a network you trust.");
	}
}

Server::~Server()
{
	// No command may run, while the rest is taken down.
	if (m_WebSocket)
	{
		m_WebSocket->Stop();
	}

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
	m_Secure = nullptr;
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

		// Not blocking, so a request to stop is noticed (Step waits a moment, if nothing has arrived).
		m_Socket->SetNonBlocking(true);

		for (;;)
		{
			if (m_StopRequested)
			{
				break;
			}

			if (!Step())
			{
				break;
			}
		}

		if (m_StopRequested)
		{
			CAM_LOG_INFO("Stopping the server...");
			break;
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

	if (m_ControlThread.joinable())
	{
		m_ControlThread.join();
	}
}

void Server::StartFramePreviews()
{
	m_FramePreviewThread = std::thread(&Server::FramePreview, std::ref(*this));
}

void Server::Stop()
{
	m_StopRequested = true;
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

	if (len == 0)
	{
		// nothing there yet
		Core::SleepMS(1);
		return true;
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

bool Server::ShouldWarnAboutRefusal(Core::addr_t addr)
{
	int64 now = Core::QueryMS();
	if (m_RefusalWarnings.size() > 1024)
	{
		m_RefusalWarnings.clear();
	}

	auto found = m_RefusalWarnings.find(addr.Value);
	if (found != m_RefusalWarnings.end() && now - found->second < 30000)
	{
		return false;
	}

	m_RefusalWarnings[addr.Value] = now;
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

	if (m_Secure)
	{
		// Only a device with the key of a camera is a camera, and it has the name, which it was given in the list of devices (it cannot pretend to be another
		// camera, nor a display).
		Core::SecureSocket::Peer peer;
		if (!m_Secure->GetPeer(clientAddr, &peer) || peer.Role != Core::DeviceRole::Camera)
		{
			if (ShouldWarnAboutRefusal(clientAddr))
			{
				CAM_LOG_WARN("{0} tried to connect as a camera, but its key is not the key of a camera. Refused.", Core::AddressToString(clientAddr));
			}

			return false;
		}

		memset(name, 0, sizeof(name));
		memcpy(name, peer.Name.c_str(), std::min<size_t>(peer.Name.size(), MAX_FRAME_NAME_LENGTH - 1));
	}

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

			// A camera, which was reported offline, is back.
			if (m_Notifier && m_OfflineCameras.erase(client->FrameTitle) > 0)
			{
				m_Notifier->AddCameraOnline(client->FrameTitle);
			}

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

	if (m_Secure)
	{
		// Only a device with the key of a display receives the pictures of the cameras (a camera key does not), under the name from the list of devices.
		Core::SecureSocket::Peer peer;
		if (!m_Secure->GetPeer(displayAddr, &peer) || peer.Role != Core::DeviceRole::Display)
		{
			if (ShouldWarnAboutRefusal(displayAddr))
			{
				CAM_LOG_WARN("{0} tried to connect as a display, but its key is not the key of a display. Refused.", Core::AddressToString(displayAddr));
			}

			return false;
		}

		memset(name, 0, sizeof(name));
		memcpy(name, peer.Name.c_str(), std::min<size_t>(peer.Name.size(), MAX_FRAME_NAME_LENGTH - 1));
	}

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

				// A camera, which vanished (and did not say goodbye), is reported. It is remembered, to report that it is back.
				if (m_Notifier && m_Config.Email.ReportCameraOffline)
				{
					m_OfflineCameras.insert((*it)->FrameTitle);
					m_Notifier->AddCameraOffline((*it)->FrameTitle, m_Config.ClientTimeoutSeconds);
				}

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
		RecordedFrame recorded;
		recorded.Data = assembly.Data;
		recorded.TimeMS = Recorder::NowMS();
		client->Frames.Push(recorded);

		// The recording on schedule (it writes on a thread of its own, this only hands the frame over).
		if (m_Recorder)
		{
			m_Recorder->AddFrame(client->CameraId, client->FrameTitle, recorded);
		}

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
			if (key == 'r' && m_Recorder)
			{
				// Saves the last minutes of all cameras (in the background, the preview must not stop).
				std::thread([this]()
				{
					uint32 files = 0;
					std::string report = SaveLastMinutes(m_Config.RecordDefaultMinutes, "", "last" + std::to_string(m_Config.RecordDefaultMinutes) + "min", &files);
					CAM_LOG_INFO("Saved the last {0} minutes (key R): {1}", m_Config.RecordDefaultMinutes, report);
				}).detach();
			}

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

void Server::StartNotifications()
{
	if (!m_Config.Email.Enabled)
	{
		return;
	}

	auto mailer = std::make_unique<Mailer>(m_Config.Email);
	if (!mailer->Validate())
	{
		CAM_LOG_ERROR("The emails are turned off, see the messages above.");
		return;
	}

	std::string recipients;
	for (const std::string &recipient : m_Config.Email.To)
	{
		recipients += (recipients.empty() ? "" : ", ") + recipient;
	}

	m_Mailer = std::move(mailer);
	m_Notifier = std::make_unique<Notifier>(*m_Mailer, m_Config.Email);

	std::string what = m_Config.Faces.Enabled ? "unknown people" : "";
	if (m_Config.Email.ReportCameraOffline)
	{
		what += (what.empty() ? "" : " and ") + std::string("cameras going offline");
	}

	std::string mode = m_Config.Email.BatchEvents ? "events within " + std::to_string(m_Config.Email.CollectSeconds) + " seconds in one email" : std::string("one email per event");
	CAM_LOG_INFO("Emails are turned on: {0} reported to {1} (via {2}:{3}), {4}, at most one email per {5} seconds.",
		what.empty() ? std::string("nothing (turn on faces or email_camera_offline)") : what, recipients, m_Config.Email.Server, m_Config.Email.Port,
		mode, m_Config.Email.MinIntervalSeconds);
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

// The picture (JPEG) with the faces drawn into it. A copy: the received frame is shared and never changed. If that does not work, the plain picture.
static EncodedFrame DrawFacesIntoJpeg(const EncodedFrame &jpeg, const std::vector<FaceResult> &faces, bool recognition, int quality)
{
	try
	{
		cv::Mat image = cv::imdecode(*jpeg, cv::IMREAD_COLOR);
		if (!image.empty())
		{
			DrawFaces(image, faces, recognition);
			auto marked = std::make_shared<std::vector<uchar>>();
			if (cv::imencode(".jpg", image, *marked, { cv::IMWRITE_JPEG_QUALITY, quality }))
			{
				return marked;
			}
		}
	}
	catch (const cv::Exception &e)
	{
		CAM_LOG_ERROR("Could not draw the faces into the picture: {}", e.what());
	}

	return jpeg;
}

void Server::FaceLoop()
{
	struct Job
	{
		uint64 Address;
		std::string Title;
		EncodedFrame Frame;
		uint32 Number;
		int64 TimeMS;
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
					// The newest frame of the buffer is the one analyzed (both are set together).
					int64 frame_ms = client->Frames.Size() > 0 ? client->Frames.At(client->Frames.Size() - 1).TimeMS : Recorder::NowMS();
					jobs.push_back({ address, client->FrameTitle, client->LatestFrame, client->LatestFrameNumber, frame_ms });
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
						if (config.DrawOnSavedClips)
						{
							client->FaceHistory.push_back({ job.TimeMS, faces });
							const int64 keep_ms = ((int64)m_Config.VideoBackupDuration + 1) * 60 * 1000;
							while (!client->FaceHistory.empty() && client->FaceHistory.back().TimeMS - client->FaceHistory.front().TimeMS > keep_ms)
							{
								client->FaceHistory.pop_front();
							}
						}

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
				// The picture with the marked face is attached to the email, even if no snapshots are stored.
				std::vector<uchar> snapshot_jpeg;
				if (m_Notifier && m_Config.Email.AttachSnapshot)
				{
					try
					{
						cv::Mat marked = frame.clone();
						DrawFaces(marked, faces, recognition);
						cv::imencode(".jpg", marked, snapshot_jpeg, { cv::IMWRITE_JPEG_QUALITY, 85 });
					}
					catch (const cv::Exception &e)
					{
						CAM_LOG_ERROR("Could not create the picture for the email: {}", e.what());
					}
				}

				NotifyUnknownPerson(job.Title, unknown_face, snapshot_file, snapshot_jpeg);

				// The video of the minutes before and during the event is kept (in the background, the analysis must go on).
				if (m_Config.RecordOnUnknownPerson && m_Recorder)
				{
					std::string camera = job.Title;
					std::thread([this, camera]()
					{
						uint32 files = 0;
						std::string report = SaveLastMinutes(m_Config.RecordEventMinutes, camera, "unknown_person", &files);
						CAM_LOG_INFO("Saved the last {0} minute(s) of {1} because of an unknown person: {2}", m_Config.RecordEventMinutes, camera, report);
					}).detach();
				}
			}
		}
	}
}

void Server::NotifyUnknownPerson(const std::string &camera, const FaceResult &face, const std::string &snapshotFile, const std::vector<uchar> &snapshotJpeg)
{
	if (!m_Notifier)
	{
		// Emails are not turned on (or not possible), the event is only in the log.
		char numbers[160];
		snprintf(numbers, sizeof(numbers), "%.2f (needed %.2f), detector score %.2f", face.Similarity, m_Config.Faces.MatchThreshold, face.Score);
		CAM_LOG_WARN("Unknown person at {0} at {1}. Best similarity to a known person: {2}. (Emails are not turned on, see email in server.cfg.)", camera, ReadableTimeString(), numbers);
		return;
	}

	// The notifier collects the events and sends them in one email, see email_collect_seconds.
	m_Notifier->AddUnknownPerson(camera, face.Similarity, m_Config.Faces.MatchThreshold, face.Score, snapshotJpeg);
}

void Server::StartRecording()
{
	m_Recorder = std::make_unique<Recorder>(m_Config.Recording);
	m_Recorder->Start();

	if (m_Config.ControlPort != 0)
	{
		m_ControlThread = std::thread(&Server::ControlLoop, std::ref(*this));
	}
}

uint32 Server::SaveReport::SavedFiles() const
{
	uint32 files = 0;
	for (const SavedClip &clip : Clips)
	{
		files += clip.Ok ? 1 : 0;
	}

	return files;
}

Server::SaveReport Server::SaveClips(uint32 minutes, const std::string &cameraFilter, const std::string &label)
{
	SaveReport report;
	if (!m_Recorder)
	{
		report.Error = "recording is not available";
		return report;
	}

	// The buffer holds the last VideoBackupDuration minutes, no more can be saved.
	uint32 available_minutes = m_Config.VideoBackupDuration;
	if (minutes == 0)
	{
		minutes = m_Config.RecordDefaultMinutes;
	}

	if (minutes > available_minutes)
	{
		report.Note = " (the server only keeps the last " + std::to_string(available_minutes) + " minutes, see backup_minutes)";
		minutes = available_minutes;
	}

	struct Clip
	{
		std::string Camera;
		std::vector<RecordedFrame> Frames;

		/// <summary>
		/// The analyses of the faces in this time (copied, so the frames can be made without the lock).
		/// </summary>
		std::vector<FaceSnapshot> Faces;
	};

	const bool draw_faces = m_FaceAnalyzer && m_Config.Faces.DrawOnSavedClips;

	std::vector<Clip> clips;
	int64 from_ms = Recorder::NowMS() - (int64)minutes * 60 * 1000;
	{
		std::lock_guard<std::mutex> lock(m_ClientsMutex);
		for (auto &client : m_Clients)
		{
			if (!cameraFilter.empty() && !EqualsIgnoreCase(cameraFilter, client->FrameTitle))
			{
				continue;
			}

			Clip clip;
			clip.Camera = client->FrameTitle;
			for (uint32 i = 0; i < client->Frames.Size(); ++i)
			{
				const RecordedFrame &frame = client->Frames.At(i);
				if (frame.TimeMS >= from_ms)
				{
					clip.Frames.push_back(frame);
				}
			}

			if (draw_faces)
			{
				clip.Faces.assign(client->FaceHistory.begin(), client->FaceHistory.end());
			}

			if (!clip.Frames.empty())
			{
				clips.push_back(std::move(clip));
			}
		}
	}

	if (clips.empty())
	{
		report.Error = cameraFilter.empty() ? "no camera has sent frames in the last " + std::to_string(minutes) + " minutes" : "the camera '" + cameraFilter + "' is not connected or has sent no frames in the last " + std::to_string(minutes) + " minutes";
		return report;
	}

	// Writing takes a moment (large files), but needs no lock anymore: the frames are shared, not copied.
	for (const Clip &clip : clips)
	{
		SavedClip saved;
		saved.Camera = clip.Camera;
		saved.Frames = (uint32)clip.Frames.size();

		// The frames get the faces of the analysis nearest in time. The analysis runs a few times per second, so a face counts for the frames around it,
		// but not longer than the analysis was stalled.
		FrameTransform transform;
		bool any_faces = false;
		for (const FaceSnapshot &snapshot : clip.Faces)
		{
			any_faces = any_faces || !snapshot.Faces.empty();
		}

		if (draw_faces && any_faces)
		{
			const bool recognition = m_FaceAnalyzer->CanRecognize();
			const int64 valid_ms = 1500;
			transform = [&clip, recognition, valid_ms](const RecordedFrame &frame) -> RecordedFrame
			{
				const std::vector<FaceSnapshot> &history = clip.Faces;
				auto next = std::lower_bound(history.begin(), history.end(), frame.TimeMS, [](const FaceSnapshot &snapshot, int64 time) { return snapshot.TimeMS < time; });
				const FaceSnapshot *nearest = nullptr;
				if (next != history.end())
				{
					nearest = &*next;
				}

				if (next != history.begin())
				{
					const FaceSnapshot *before = &*(next - 1);
					if (!nearest || frame.TimeMS - before->TimeMS <= nearest->TimeMS - frame.TimeMS)
					{
						nearest = before;
					}
				}

				if (!nearest || nearest->Faces.empty() || std::abs(nearest->TimeMS - frame.TimeMS) > valid_ms || !frame.Data)
				{
					return frame;
				}

				RecordedFrame marked = frame;
				marked.Data = DrawFacesIntoJpeg(frame.Data, nearest->Faces, recognition, 85);
				return marked;
			};
		}

		saved.Ok = m_Recorder->SaveClip(clip.Camera, clip.Frames, label, &saved.Path, &saved.Error, transform);
		report.Clips.push_back(saved);
	}

	return report;
}

std::string Server::SaveLastMinutes(uint32 minutes, const std::string &cameraFilter, const std::string &label, uint32 *savedFiles)
{
	SaveReport report = SaveClips(minutes, cameraFilter, label);
	*savedFiles = report.SavedFiles();
	if (!report.Error.empty())
	{
		return report.Error;
	}

	std::string text;
	for (const SavedClip &clip : report.Clips)
	{
		text += (text.empty() ? "" : "; ") + clip.Camera + ": " + (clip.Ok ? clip.Path + " (" + std::to_string(clip.Frames) + " frames)" : "FAILED, " + clip.Error);
	}

	return text + report.Note;
}

void Server::ControlLoop()
{
	Core::Socket *socket = Core::Socket::Create();
	if (!socket->Open() || !socket->BindLoopback(m_Config.ControlPort))
	{
		CAM_LOG_ERROR("Could not open the control port {} (is another server running?), the commands (CamServer --record_now, ...) do not work.", m_Config.ControlPort);
		delete socket;
		return;
	}

	socket->SetNonBlocking(true);
	CAM_LOG_INFO("Control port {} (only reachable from this computer): CamServer --record_now=MINUTES saves the last minutes, --control_status, --control_stop.", m_Config.ControlPort);

	static Byte BUF[2048];
	while (m_Running && !m_StopRequested)
	{
		Core::addr_t sender = {};
		int32 len = socket->Recv(BUF, sizeof(BUF) - 1, &sender);
		if (len <= 0)
		{
			Core::SleepMS(20);
			continue;
		}

		BUF[len] = 0;
		std::string command((const char *)BUF);
		while (!command.empty() && (command.back() == 10 || command.back() == 13 || command.back() == 32))
		{
			command.pop_back();
		}

		std::string reply;
		if (command.rfind("save ", 0) == 0)
		{
			// save <minutes> [camera name]
			std::string rest = command.substr(5);
			size_t space = rest.find(' ');
			int minutes = atoi(rest.substr(0, space).c_str());
			std::string camera = space == std::string::npos ? "" : rest.substr(space + 1);
			if (minutes <= 0)
			{
				reply = "ERROR: the number of minutes is missing";
			}
			else
			{
				uint32 files = 0;
				std::string report = SaveLastMinutes((uint32)minutes, camera, "last" + std::to_string(minutes) + "min", &files);
				reply = (files > 0 ? "OK " : "ERROR ") + std::to_string(files) + " file(s): " + report;
				CAM_LOG_INFO("Saved the last {0} minute(s) on request: {1}", minutes, report);
			}
		}
		else if (command == "status")
		{
			reply = "Cameras:\n";
			{
				std::lock_guard<std::mutex> lock(m_ClientsMutex);
				for (auto &client : m_Clients)
				{
					uint32 frames = client->Frames.Size();
					double seconds = frames > 1 ? (client->Frames.At(frames - 1).TimeMS - client->Frames.At(0).TimeMS) / 1000.0 : 0.0;
					char line[160];
					snprintf(line, sizeof(line), "  %s: %u frames buffered (%.0f seconds)\n", client->FrameTitle.c_str(), frames, seconds);
					reply += line;
				}

				if (m_Clients.empty())
				{
					reply += "  (none connected)\n";
				}

				reply += "Displays connected: " + std::to_string(m_Displays.size()) + "\n";
			}

			reply += "Recordings folder: " + m_Config.Recording.Path + "\n";
			reply += "Scheduled recording: " + (m_Recorder && m_Recorder->IsScheduled() ? m_Config.Recording.Schedule : std::string("off")) + "\n";
			reply += "Buffer: the last " + std::to_string(m_Config.VideoBackupDuration) + " minutes per camera";
		}
		else if (command == "stop")
		{
			reply = "OK, the server is stopping";
			CAM_LOG_INFO("Stop requested through the control port.");
			m_StopRequested = true;
		}
		else
		{
			reply = "ERROR: unknown command (save <minutes> [camera], status, stop)";
		}

		socket->Send(reply.c_str(), (int32)reply.size(), sender);
	}

	delete socket;
}

// ============================================================================================================================ remote control

std::string LoadOrCreateWebSocketToken(const ServerConfig &config, std::string *error)
{
	if (!config.WebSocketToken.empty())
	{
		return config.WebSocketToken;
	}

	Core::FileSystem *fs = Core::FileSystem::Get();
	std::string content;
	if (fs->FileExists(config.WebSocketTokenFile) && fs->ReadTextFile(config.WebSocketTokenFile, &content) > 0)
	{
		while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' '))
		{
			content.pop_back();
		}

		if (content.size() >= 16)
		{
			return content;
		}
	}

	// The first start: a new secret of 256 random bits.
	Byte random[32];
	if (!Core::crypto::RandomBytes(random, sizeof(random)))
	{
		*error = "the system could not make random numbers";
		return "";
	}

	std::string token = Core::BytesToHex(random, sizeof(random));
	if (!fs->WriteTextFile(config.WebSocketTokenFile, token + "\n"))
	{
		*error = "could not write " + config.WebSocketTokenFile;
		return "";
	}

#ifndef CAM_PLATFORM_WINDOWS
	// Only the owner may read the secret.
	std::error_code permission_error;
	std::filesystem::permissions(config.WebSocketTokenFile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, permission_error);
#endif

	return token;
}

void Server::StartRemoteControl()
{
	if (m_Config.WebSocketPort == 0)
	{
		return;
	}

	std::string token_error;
	std::string token = LoadOrCreateWebSocketToken(m_Config, &token_error);
	if (token.empty())
	{
		CAM_LOG_ERROR("The remote control (WebSocket) is not started: {}", token_error);
		return;
	}

	m_Commands = std::make_unique<Core::CommandDispatcher>(token);
	RegisterCommands();

	Core::WebSocketServerConfig ws_config;
	ws_config.BindAddress = m_Config.WebSocketBind;
	ws_config.Port = m_Config.WebSocketPort;
	m_WebSocket = std::make_unique<Core::WebSocketServer>(ws_config, m_Commands->AsMessageHandler());
	if (!m_WebSocket->Start())
	{
		CAM_LOG_ERROR("Could not open the port {0} of the remote control (WebSocket) on {1} (is another server running?).", m_Config.WebSocketPort, m_Config.WebSocketBind);
		m_WebSocket.reset();
		return;
	}

	CAM_LOG_INFO("Remote control: ws://{0}:{1} {2}. Log in with the token from {3} (CamServer --show_websocket_token).", m_Config.WebSocketBind, m_Config.WebSocketPort, m_Config.WebSocketBind == "127.0.0.1" ? "(only programs on this computer)" : "(the network! the connection is not encrypted)", m_Config.WebSocketTokenFile);
}

void Server::RegisterCommands()
{
	using Core::Json;
	using Context = Core::CommandDispatcher::Context;

	// Lists the cameras (the caller holds m_ClientsMutex).
	auto camera_list = [this]()
	{
		Json cameras = Json::Array();
		auto now = std::chrono::steady_clock::now();
		for (auto &client : m_Clients)
		{
			uint32 frames = client->Frames.Size();
			double seconds = frames > 1 ? (client->Frames.At(frames - 1).TimeMS - client->Frames.At(0).TimeMS) / 1000.0 : 0.0;

			Json camera = Json::Object();
			camera["name"] = client->FrameTitle;
			camera["address"] = Core::AddressToString(client->Address);
			camera["frames_buffered"] = frames;
			camera["seconds_buffered"] = seconds;
			camera["last_seen_ms_ago"] = (int64)std::chrono::duration_cast<std::chrono::milliseconds>(now - client->LastSeen).count();
			camera["faces_visible"] = (uint32)client->Faces.size();
			cameras.Push(camera);
		}

		return cameras;
	};

	m_Commands->Register("status", "the state of the server: cameras, displays, recording settings, the devices", [this, camera_list](const Json &, const Context &, Json *result, std::string *)
	{
		{
			std::lock_guard<std::mutex> lock(m_ClientsMutex);
			(*result)["cameras"] = camera_list();
			(*result)["displays"] = (uint32)m_Displays.size();
		}

		(*result)["protocol_version"] = (uint32)m_Version;
		(*result)["buffer_minutes"] = m_Config.VideoBackupDuration;
		(*result)["device_keys_required"] = m_Devices != nullptr;
		if (m_Devices)
		{
			(*result)["devices"] = m_Devices->Count();
		}

		Json recording = Json::Object();
		recording["folder"] = m_Config.Recording.Path;
		recording["scheduled"] = m_Recorder && m_Recorder->IsScheduled();
		recording["schedule"] = m_Config.Recording.Schedule;
		recording["default_minutes"] = m_Config.RecordDefaultMinutes;
		(*result)["recording"] = recording;
		return true;
	});

	m_Commands->Register("cameras", "the connected cameras, with how much video is buffered of each", [this, camera_list](const Json &, const Context &, Json *result, std::string *)
	{
		std::lock_guard<std::mutex> lock(m_ClientsMutex);
		(*result)["cameras"] = camera_list();
		return true;
	});

	m_Commands->Register("displays", "the connected displays", [this](const Json &, const Context &, Json *result, std::string *)
	{
		Json displays = Json::Array();
		std::lock_guard<std::mutex> lock(m_ClientsMutex);
		for (auto &display : m_Displays)
		{
			Json item = Json::Object();
			item["name"] = display->Name;
			item["address"] = Core::AddressToString(display->Address);
			item["camera"] = display->CameraFilter;
			item["max_fps"] = display->MaxFPS;
			displays.Push(item);
		}

		(*result)["displays"] = displays;
		return true;
	});

	m_Commands->Register("save", "saves the last minutes of the buffer of one or all cameras as video files. args: camera (name, empty or missing = all cameras), minutes (default: record_default_minutes), label (part of the file name)",
		[this](const Json &args, const Context &, Json *result, std::string *error)
	{
		std::string camera = args.Get("camera").AsString();
		int64 minutes = args.Get("minutes").AsInt(0);
		if (minutes < 0 || minutes > 24 * 60)
		{
			*error = "minutes must be a number between 1 and 1440";
			return false;
		}

		std::string label = args.Get("label").AsString();
		if (label.empty())
		{
			label = "remote";
		}

		// The recorder makes the label safe for a file name.
		SaveReport report = SaveClips((uint32)minutes, camera, label);
		if (!report.Error.empty())
		{
			*error = report.Error;
			return false;
		}

		Json clips = Json::Array();
		for (const SavedClip &clip : report.Clips)
		{
			Json item = Json::Object();
			item["camera"] = clip.Camera;
			item["ok"] = clip.Ok;
			item["frames"] = clip.Frames;
			if (clip.Ok)
			{
				std::error_code path_error;
				item["file"] = std::filesystem::path(clip.Path).generic_string();
				item["absolute_path"] = std::filesystem::absolute(clip.Path, path_error).generic_string();
			}
			else
			{
				item["error"] = clip.Error;
			}

			clips.Push(item);
		}

		(*result)["saved"] = report.SavedFiles();
		(*result)["clips"] = clips;
		if (!report.Note.empty())
		{
			(*result)["note"] = report.Note;
		}

		CAM_LOG_INFO("Saved {0} video(s) on request of the remote control.", report.SavedFiles());
		return report.SavedFiles() > 0;
	});

	m_Commands->Register("snapshot", "the newest picture of a camera as a JPEG (Base64), for a live view, as the displays get it (faces and names drawn into it). args: camera (name), after (the frame number, which the caller has already; if there is no newer picture, the answer has \"unchanged\": true and no picture), faces (default true; false: the plain picture)",
		[this](const Json &args, const Context &, Json *result, std::string *error)
	{
		std::string camera = args.Get("camera").AsString();
		int64 after = args.Get("after").AsInt(0);

		// The same picture as on the displays: with the faces (and the names of the known people) drawn into it, as long as the analysis is up to date.
		bool want_faces = args.Get("faces").AsBool(true) && m_FaceAnalyzer && m_Config.Faces.DrawOnDisplays;
		const int64 faces_valid_ms = 2000;

		EncodedFrame frame;
		uint32 frame_number = 0;
		std::vector<FaceResult> faces;
		{
			std::lock_guard<std::mutex> lock(m_ClientsMutex);
			for (auto &client : m_Clients)
			{
				if (client->FrameTitle == camera)
				{
					frame = client->LatestFrame;
					frame_number = client->LatestFrameNumber;
					if (want_faces && Core::QueryMS() - client->FacesUpdatedMS <= faces_valid_ms)
					{
						faces = client->Faces;
					}

					break;
				}
			}
		}

		if (frame_number == 0 || !frame || frame->empty())
		{
			*error = "there is no picture of the camera \"" + camera + "\" (it is not connected, or has not sent a picture yet)";
			return false;
		}

		(*result)["camera"] = camera;
		(*result)["frame"] = frame_number;
		if ((int64)frame_number == after)
		{
			(*result)["unchanged"] = true;
			return true;
		}

		if (!faces.empty())
		{
			frame = DrawFacesIntoJpeg(frame, faces, m_FaceAnalyzer->CanRecognize(), 80);
		}

		// Encoded outside of the lock.
		(*result)["jpeg"] = Core::websocket::Base64Encode((const Byte *)frame->data(), (uint32)frame->size());
		(*result)["faces"] = (uint32)faces.size();
		return true;
	});

	m_Commands->Register("recordings", "the newest video files in the recordings folder. args: limit (default 50, at most 500)",
		[this](const Json &args, const Context &, Json *result, std::string *error)
	{
		int64 limit = std::clamp<int64>(args.Get("limit").AsInt(50), 1, 500);

		struct Entry
		{
			std::string Relative;
			uint64 Bytes;
			int64 Modified;
		};

		std::vector<Entry> entries;
		std::error_code fs_error;
		std::filesystem::path root(m_Config.Recording.Path);
		if (std::filesystem::is_directory(root, fs_error))
		{
			for (std::filesystem::recursive_directory_iterator it(root, fs_error), end; !fs_error && it != end; it.increment(fs_error))
			{
				std::error_code entry_error;
				if (!it->is_regular_file(entry_error) || it->path().extension() != ".avi")
				{
					continue;
				}

				Entry entry;
				entry.Relative = std::filesystem::relative(it->path(), root, entry_error).generic_string();
				entry.Bytes = (uint64)it->file_size(entry_error);
				// The clock of the file system has its own start (1601 on Windows), converted to seconds since 1970.
				auto file_time = it->last_write_time(entry_error);
				auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
				entry.Modified = (int64)std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
				entries.push_back(entry);
			}
		}

		std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.Modified > b.Modified; });

		Json files = Json::Array();
		for (size_t i = 0; i < entries.size() && (int64)i < limit; ++i)
		{
			Json item = Json::Object();
			item["file"] = entries[i].Relative;
			item["bytes"] = entries[i].Bytes;
			item["modified"] = entries[i].Modified;		// seconds since 1970
			files.Push(item);
		}

		(*result)["folder"] = m_Config.Recording.Path;
		(*result)["total"] = (uint32)entries.size();
		(*result)["files"] = files;
		(void)error;
		return true;
	});

	m_Commands->Register("devices", "the cameras and displays, which have a key (never the keys themselves)", [this](const Json &, const Context &, Json *result, std::string *error)
	{
		if (!m_Devices)
		{
			*error = "device keys are turned off (auth = false)";
			return false;
		}

		Json devices = Json::Array();
		for (const Core::Device &device : m_Devices->List())
		{
			Json item = Json::Object();
			item["name"] = device.Name;
			item["role"] = Core::DeviceRoleName(device.Role);
			item["key_id"] = Core::KeyIdToString(device.KeyId);
			devices.Push(item);
		}

		(*result)["devices"] = devices;
		return true;
	});

	m_Commands->Register("device_add", "makes a key for a new camera or display. args: role (camera or display), name. The key is only handed out to programs on the computer of the server",
		[this](const Json &args, const Context &context, Json *result, std::string *error)
	{
		if (!m_Devices)
		{
			*error = "device keys are turned off (auth = false)";
			return false;
		}

		if (!context.IsLoopback)
		{
			// The connection is not encrypted, a key must not be sent over the network.
			*error = "a key is only handed out to a program on the computer of the server (use CamServer --add_device there)";
			return false;
		}

		Core::DeviceRole role = Core::DeviceRoleFromName(args.Get("role").AsString());
		Core::Device device;
		if (role == Core::DeviceRole::None || !m_Devices->Add(role, args.Get("name").AsString(), &device, error))
		{
			if (role == Core::DeviceRole::None)
			{
				*error = "role must be camera or display";
			}

			return false;
		}

		(*result)["name"] = device.Name;
		(*result)["role"] = Core::DeviceRoleName(device.Role);
		(*result)["key"] = Core::BytesToHex(device.Key, Core::crypto::KEY_BYTES);
		(*result)["key_id"] = Core::KeyIdToString(device.KeyId);
		CAM_LOG_INFO("The {0} '{1}' was added on request of the remote control.", Core::DeviceRoleName(device.Role), device.Name);
		return true;
	});

	m_Commands->Register("device_remove", "takes the key of a device away, it is disconnected. args: name",
		[this](const Json &args, const Context &, Json *result, std::string *error)
	{
		if (!m_Devices)
		{
			*error = "device keys are turned off (auth = false)";
			return false;
		}

		std::string name = args.Get("name").AsString();
		if (!m_Devices->Remove(name, error))
		{
			return false;
		}

		// The connection of the device ends at once (the secure socket checks the list).
		if (m_Secure)
		{
			m_Secure->UpdateDevices();
		}

		(*result)["removed"] = name;
		CAM_LOG_INFO("The device '{0}' was removed on request of the remote control.", name);
		return true;
	});

	m_Commands->Register("stop", "stops the server (the videos, which are being recorded, are finished first)", [this](const Json &, const Context &, Json *result, std::string *)
	{
		CAM_LOG_INFO("Stop requested through the remote control.");
		m_StopRequested = true;
		(*result)["stopping"] = true;
		return true;
	});
}
