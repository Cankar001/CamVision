#include "Client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#include "Core/Log.h"

// How long to wait for the server to accept the connection, before giving up.
#define CONNECT_TIMEOUT_MS 10000

// How often the connection request is repeated while waiting for the server (the datagram might get lost).
#define CONNECT_RESEND_INTERVAL_MS 500

// How long to wait for the server to confirm that the connection was closed, before giving up.
#define DISCONNECT_TIMEOUT_MS 2000

// Quality of the JPEG compression for every frame sent to the server (0 - 100).
#define FRAME_JPEG_QUALITY 80

// Size of the kernel socket buffers. Large enough to hold a burst of datagrams for several frames.
#define SOCKET_BUFFER_SIZE (4 * 1024 * 1024)

Client::Client(const ClientConfig &config)
	: m_Config(config), m_Camera(false, 1280, 720)
{
	std::string cwd = "";
	Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);
	m_Version = Core::utils::GetLocalVersion("../../..");

	CAM_LOG_INFO("===================== CONFIG ===================================");
	CAM_LOG_INFO("IP                    : {}", config.ServerIP);
	CAM_LOG_INFO("Port                  : {}", config.Port);
	CAM_LOG_INFO("Current Client version: {}", m_Version);
	CAM_LOG_INFO("Current CWD           : {}", cwd);
	CAM_LOG_INFO("================================================================");

	m_Socket = Core::Socket::Create();
	if (!m_Socket->Open(true, m_Config.ServerIP, m_Config.Port))
	{
		CAM_LOG_ERROR("Socket could not be opened!");
	}
	if (!m_Socket->SetNonBlocking(true))
	{
		CAM_LOG_ERROR("Socket could not be set to non-blocking!");
	}
	if (!m_Socket->SetBufferSizes(SOCKET_BUFFER_SIZE, SOCKET_BUFFER_SIZE))
	{
		CAM_LOG_ERROR("Socket buffer sizes could not be set!");
	}

	m_Host = m_Socket->Lookup(m_Config.ServerIP, m_Config.Port);
}

Client::~Client()
{
	Release();
}

void Client::Release()
{
	CAM_LOG_INFO("Releasing all resources.");

	if (m_CameraThread.joinable())
		m_CameraThread.join();

	if (m_NetworkThread.joinable())
		m_NetworkThread.join();

	m_Camera.Release();

	if (m_Socket)
	{
		CAM_LOG_INFO("Releasing socket");
		delete m_Socket;
		m_Socket = nullptr;
	}
}

void Client::Run(bool shouldShowFrames)
{
	m_NetworkThread = std::thread(&Client::NetworkLoop, std::ref(*this));
	m_CameraThread = std::thread(&Client::CameraLoop, std::ref(*this));

	if (shouldShowFrames)
	{
		Show();
	}
	else
	{
		while (m_Running)
		{
		}
	}

	// wait until the network thread is finished
	while (!m_NetworkThreadFinished)
	{
	}
}

void Client::NetworkLoop()
{
	using Clock = std::chrono::steady_clock;
	auto since_ms = [](Clock::time_point t) { return (int64)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); };

	ClientConnectionStartMessage msg = {};
	msg.Header.Type = CLIENT_CONNECTION_START;
	msg.Header.Version = m_Version;
	memcpy(msg.FrameName, "Client #1", sizeof("Client #1"));
	msg.FPS = 30;

	Clock::time_point connect_start = Clock::now();
	Clock::time_point last_connect_request = Clock::now() - std::chrono::milliseconds(CONNECT_RESEND_INTERVAL_MS);
	Clock::time_point close_start;

	static Byte BUF[65536];
	while (true)
	{
		if (!m_ConnectedToServer)
		{
			if (since_ms(connect_start) >= CONNECT_TIMEOUT_MS)
			{
				CAM_LOG_ERROR("Fatal error: Could not connect to server!");
				m_Running = false;
				m_NetworkThreadFinished = true;
				return;
			}

			// The request (or the response) might get lost, so repeat it until the server answers.
			if (since_ms(last_connect_request) >= CONNECT_RESEND_INTERVAL_MS)
			{
				CAM_LOG_DEBUG("Sending connection start request to server...");
				m_Socket->Send(&msg, sizeof(msg), m_Host);
				last_connect_request = Clock::now();
			}
		}

		if (!m_Running && !m_SentConnectionCloseRequest)
		{
			// send request to server, that we want to close the connection
			CAM_LOG_DEBUG("Sending connection close request to server...");
			ClientConnectionCloseMessage close_msg = {};
			close_msg.Header.Type = CLIENT_CONNECTION_CLOSE;
			close_msg.Header.Version = m_Version;
			m_Socket->Send(&close_msg, sizeof(close_msg), m_Host);
			m_SentConnectionCloseRequest = true;
			close_start = Clock::now();
		}

		if (m_SentConnectionCloseRequest && since_ms(close_start) >= DISCONNECT_TIMEOUT_MS)
		{
			CAM_LOG_ERROR("Server did not confirm the connection close request.");
			m_NetworkThreadFinished = true;
			return;
		}

		Core::addr_t addr = {};
		int32 len = m_Socket->Recv(BUF, sizeof(BUF), &addr);
		if (len < 0)
		{
			CAM_LOG_ERROR("Failed to receive data from network layer!");
			Core::SleepMS(10);
			continue;
		}

		if (len == 0)
		{
			// Nothing received yet.
			Core::SleepMS(1);
			continue;
		}

		// ignore all messages from unknown senders
		if (addr.Value != m_Host.Value)
		{
			CAM_LOG_ERROR("Unknown sender!");
			continue;
		}

		if (len < sizeof(header_t))
		{
			CAM_LOG_ERROR("Unexpected header size!");
			continue;
		}

		header_t *header = (header_t *)BUF;

		if (header->Version != m_Version)
		{
			CAM_LOG_ERROR("Unexpected version encountered.");
			continue;

		}

		bool message_success = false;
		switch (header->Type)
		{
			case SERVER_CONNECTION_START:
				message_success = OnConnectionAccepted(BUF, len);
				break;

			case SERVER_CONNECTION_CLOSE:
				message_success = OnConnectionClosed(BUF, len);
				break;

		}

		if (!message_success)
		{
			CAM_LOG_ERROR("Specific message handler failed. Aborting.");
			break;
		}

		// If we handled the server connecection close request successfully, quit from the network thread.
		if (header->Type == SERVER_CONNECTION_CLOSE && message_success)
		{
			m_NetworkThreadFinished = true;
			CAM_LOG_INFO("We got the connection close response from the server and it was successful. Closing Connection.");
			break;
		}
	}
}

void Client::CameraLoop()
{
	while (m_Running)
	{
		if (!m_Camera.IsRunning())
		{
			m_Running = false;
			break;
		}

		m_Camera.GenerateFrames();
	}
}

void Client::Show()
{
	while (m_Running)
	{
		if (!m_Camera.IsRunning())
		{
			m_Running = false;
			break;
		}

		uint32 frame_size = 0;
		uint32 frame_width = 0;
		uint32 frame_height = 0;
		Byte *frame = m_Camera.ShowLive(&frame_size, &frame_width, &frame_height);

		if (!frame)
		{
			CAM_LOG_ERROR("Could not read image from camera!");
			continue;
		}

		ProcessFrame(frame, frame_size, frame_width, frame_height);
		SendFrameToServer(frame, frame_size, frame_width, frame_height);

		delete[] frame;
	}
}

bool Client::OnConnectionAccepted(Byte *message, uint32 length)
{
	if (length != sizeof(ServerConnectionStartResponse))
	{
		CAM_LOG_ERROR("Unexpected message size encountered.");
		return false;
	}

	CAM_LOG_DEBUG("Trying to connect to server...");
	ServerConnectionStartResponse *msg = (ServerConnectionStartResponse *)message;

	if (!msg->ConnectionAccepted)
	{
		CAM_LOG_ERROR("Server responded unsuccessful to connection start request!");
		return false;
	}

	m_ConnectedToServer = true;
	CAM_LOG_INFO("Connected to server successfully!");
	return true;
}

bool Client::OnConnectionClosed(Byte *message, uint32 length)
{
	if (length != sizeof(ServerConnectionCloseResponse))
	{
		CAM_LOG_ERROR("Unexpected message size encountered.");
		return false;
	}

	CAM_LOG_DEBUG("Trying to disconnect from server...");
	ServerConnectionCloseResponse *msg = (ServerConnectionCloseResponse *)message;

	if (!msg->ConnectionClosed)
	{
		CAM_LOG_ERROR("Server responded unsuccessful to connection close request!");
		return false;
	}

	CAM_LOG_INFO("Disconnected from server successfully!");
	return true;
}

void Client::ProcessFrame(Byte *frame, uint32 frame_size, uint32 frame_width, uint32 frame_height)
{
	// TODO
}

void Client::SendFrameToServer(Byte *frame, uint32 frame_size, uint32 frame_width, uint32 frame_height)
{
	if (!m_ConnectedToServer)
	{
		// The server does not know this client yet, it would just drop the frame.
		return;
	}

	cv::Mat image((int32)frame_height, (int32)frame_width, m_Camera.GetFormat(), frame);
	if (image.total() * image.elemSize() != frame_size)
	{
		CAM_LOG_ERROR("Frame size does not match the camera format, dropping frame.");
		return;
	}

	static const std::vector<int32> params = { cv::IMWRITE_JPEG_QUALITY, FRAME_JPEG_QUALITY };
	if (!cv::imencode(".jpg", image, m_EncodeBuffer, params))
	{
		CAM_LOG_ERROR("Could not JPEG encode frame, dropping frame.");
		return;
	}

	uint32 encoded_size = (uint32)m_EncodeBuffer.size();
	uint32 chunk_count = (encoded_size + FRAME_CHUNK_PAYLOAD_SIZE - 1) / FRAME_CHUNK_PAYLOAD_SIZE;
	if (encoded_size == 0 || encoded_size > MAX_ENCODED_FRAME_SIZE || chunk_count > 0xFFFF)
	{
		CAM_LOG_ERROR("Encoded frame has an invalid size of {} bytes, dropping frame.", encoded_size);
		return;
	}

	Byte packet[sizeof(ClientFrameChunkMessage) + FRAME_CHUNK_PAYLOAD_SIZE];
	ClientFrameChunkMessage *chunk = (ClientFrameChunkMessage *)packet;
	chunk->Header.Type = CLIENT_FRAME;
	chunk->Header.Version = m_Version;
	chunk->FrameId = m_NextFrameId++;
	chunk->FrameSize = encoded_size;
	chunk->ChunkCount = (uint16)chunk_count;

	for (uint32 i = 0; i < chunk_count; ++i)
	{
		uint32 offset = i * FRAME_CHUNK_PAYLOAD_SIZE;
		uint32 payload_size = std::min(FRAME_CHUNK_PAYLOAD_SIZE, encoded_size - offset);
		uint32 packet_size = sizeof(ClientFrameChunkMessage) + payload_size;

		chunk->ChunkIndex = (uint16)i;
		memcpy(packet + sizeof(ClientFrameChunkMessage), m_EncodeBuffer.data() + offset, payload_size);

		if (m_Socket->Send(packet, packet_size, m_Host) != (int32)packet_size)
		{
			// Frames are never retransmitted, the server drops the incomplete frame as soon as a newer one arrives.
			CAM_LOG_ERROR("Could not send chunk {0}/{1} of frame {2}, dropping rest of frame.", i + 1, chunk_count, chunk->FrameId);
			return;
		}
	}
}
