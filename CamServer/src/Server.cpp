#include "Server.h"

#include <algorithm>
#include <cstring>
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

	m_Clients.clear();

	delete m_Socket;
	m_Socket = nullptr;
}

void Server::Run()
{
	m_Running = true;
	CAM_LOG_INFO("Waiting for clients to connect...");

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
		if (!FindClient(clientAddr))
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
		// Unknown sender (e.g. not connected yet), just ignore the frame.
		return true;
	}

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
	};

	std::unordered_map<uint64, uint32> shown_frames;
	std::unordered_set<std::string> created_windows;

	while (m_Running)
	{
		// Only hold the lock while copying the (shared) frame pointers, decoding and drawing happens without it.
		std::vector<Preview> previews;
		{
			std::lock_guard<std::mutex> lock(m_ClientsMutex);
			for (auto &client : m_Clients)
			{
				uint32 &shown = shown_frames[client->Address.Value];
				if (client->LatestFrame && client->LatestFrameNumber != shown)
				{
					shown = client->LatestFrameNumber;
					previews.push_back({ client->FrameTitle, client->LatestFrame });
				}
			}
		}

		for (Preview &preview : previews)
		{
			cv::Mat frame = cv::imdecode(*preview.Frame, cv::IMREAD_COLOR);
			if (frame.empty())
			{
				CAM_LOG_ERROR("Could not decode frame of {}!", preview.Name);
				continue;
			}

			if (created_windows.insert(preview.Name).second)
			{
				// Resizable window, which keeps the aspect ratio of the frame instead of stretching it. Starts with the size of the frame.
				cv::namedWindow(preview.Name.c_str(), cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
				cv::resizeWindow(preview.Name.c_str(), frame.cols, frame.rows);
			}

			cv::imshow(preview.Name.c_str(), frame);
		}

		char key = (char)cv::waitKey(1);
		if (key == 'q')
		{
			cv::destroyAllWindows();
			created_windows.clear();
		}
	}
}
