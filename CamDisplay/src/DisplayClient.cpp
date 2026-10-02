#include "DisplayClient.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Core/Log.h"

// If nothing arrives from the server for this long, the display connects again (the server may have been restarted).
#define SERVER_SILENCE_TIMEOUT_MS 6000

// How often a display, which is not connected yet, repeats its request.
#define CONNECT_RESEND_INTERVAL_MS 500

// How often a connected display tells the server, that it is still there. Must be well below the timeout of the server.
#define HEARTBEAT_INTERVAL_MS 1000

// A camera, which did not send anything for this long, is shown as "no signal"...
#define NO_SIGNAL_AFTER_MS 5000

// ... and disappears completely after this time (the camera is gone).
#define REMOVE_CAMERA_AFTER_MS 30000

static const char *WINDOW_NAME = "CamVision";

DisplayClient::DisplayClient(const DisplayClientConfig &config)
	: m_Config(config)
{
	CAM_LOG_INFO("===================== CONFIG ===================================");
	CAM_LOG_INFO("Server                : {0}:{1}", config.ServerIP, config.Port);
	CAM_LOG_INFO("Display name          : {}", config.Name);
	CAM_LOG_INFO("Camera                : {}", config.Camera.empty() ? std::string("all cameras") : config.Camera);
	CAM_LOG_INFO("Max FPS per camera    : {}", config.MaxFPS);
	CAM_LOG_INFO("Protocol version      : {}", CAM_PROTOCOL_VERSION);
	CAM_LOG_INFO("================================================================");

	m_Socket = Core::Socket::Create();
	if (!m_Socket->Open(true, config.ServerIP, config.Port))
	{
		CAM_LOG_ERROR("Socket could not be opened!");
	}

	if (!m_Socket->SetNonBlocking(true))
	{
		CAM_LOG_ERROR("Socket could not be set to non-blocking!");
	}

	// The frames arrive in bursts of datagrams, a larger buffer keeps them from getting lost, while they wait to be processed.
	m_Socket->SetBufferSizes(256 * 1024, 4 * 1024 * 1024);

	m_Host = m_Socket->Lookup(config.ServerIP, config.Port);
}

DisplayClient::~DisplayClient()
{
	m_Running = false;
	if (m_NetworkThread.joinable())
	{
		m_NetworkThread.join();
	}

	delete m_Socket;
	m_Socket = nullptr;
}

void DisplayClient::Stop()
{
	m_Running = false;
}

int DisplayClient::Run()
{
	if (m_Host.Value == 0)
	{
		CAM_LOG_ERROR("The address of the server {} could not be resolved!", m_Config.ServerIP);
		return 1;
	}

	m_Running = true;
	m_NetworkThread = std::thread(&DisplayClient::NetworkLoop, this);

	bool snapshot_mode = !m_Config.SnapshotFile.empty();
	if (!snapshot_mode)
	{
		try
		{
			// The display always covers the whole screen, without a border or a title bar.
			cv::namedWindow(WINDOW_NAME, cv::WINDOW_NORMAL);
			cv::setWindowProperty(WINDOW_NAME, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
		}
		catch (const cv::Exception &e)
		{
			CAM_LOG_ERROR("Could not open the window: {}", e.what());
			m_Running = false;
			return 1;
		}
	}

	int exit_code = 0;
	int64 start_ms = Core::QueryMS();

	while (m_Running)
	{
		// Everything of OpenCV, which may fail (for example if the window was closed), must not end the program with an exception.
		try
		{
			int width = (int)m_Config.WindowWidth;
			int height = (int)m_Config.WindowHeight;
			if (!snapshot_mode)
			{
				// The picture has exactly the size of the screen (the fullscreen window), so nothing is stretched.
				cv::Rect area = cv::getWindowImageRect(WINDOW_NAME);
				if (area.width > 16 && area.height > 16)
				{
					width = area.width;
					height = area.height;
				}
			}

			uint32 fresh_cameras = 0;
			cv::Mat canvas = Compose(width, height, &fresh_cameras);

			if (snapshot_mode)
			{
				if (fresh_cameras >= m_Config.SnapshotMinCameras)
				{
					if (cv::imwrite(m_Config.SnapshotFile, canvas))
					{
						CAM_LOG_INFO("Stored the picture of the display in {}", m_Config.SnapshotFile);
					}
					else
					{
						CAM_LOG_ERROR("Could not store the picture in {}", m_Config.SnapshotFile);
						exit_code = 1;
					}

					break;
				}

				if (Core::QueryMS() - start_ms > (int64)m_Config.SnapshotTimeoutSeconds * 1000)
				{
					CAM_LOG_ERROR("Pictures of {0} camera(s) were not received within {1} seconds.", m_Config.SnapshotMinCameras, m_Config.SnapshotTimeoutSeconds);
					exit_code = 1;
					break;
				}

				Core::SleepMS(50);
				continue;
			}

			cv::imshow(WINDOW_NAME, canvas);
			// Esc (or Q) shuts the display down. On some systems waitKey returns more than the key code in the higher bits.
			int key = cv::waitKey(15) & 0xFF;
			if (key == 27 || key == 'q' || key == 'Q')
			{
				CAM_LOG_INFO("Shutting down.");
				break;
			}

			// The window was closed (for example by the window manager).
			if (cv::getWindowProperty(WINDOW_NAME, cv::WND_PROP_VISIBLE) < 1)
			{
				break;
			}
		}
		catch (const cv::Exception &e)
		{
			CAM_LOG_ERROR("Display error: {}", e.what());
			Core::SleepMS(100);
		}
	}

	m_Running = false;
	if (m_NetworkThread.joinable())
	{
		m_NetworkThread.join();
	}

	return exit_code;
}

void DisplayClient::NetworkLoop()
{
	static Byte BUF[65536];

	int64 last_request_ms = 0;
	int64 last_heartbeat_ms = 0;
	int64 last_receive_ms = Core::QueryMS();
	bool announced_connecting = false;

	while (m_Running)
	{
		int64 now = Core::QueryMS();

		// If nothing arrives anymore (not even a frame), the server may have been restarted and forgot us. Registering again does no harm.
		if (m_Connected && now - last_receive_ms > SERVER_SILENCE_TIMEOUT_MS)
		{
			CAM_LOG_WARN("The server does not send anything anymore, connecting again...");
			m_Connected = false;
			announced_connecting = false;
		}

		if (!m_Connected)
		{
			if (now - last_request_ms >= CONNECT_RESEND_INTERVAL_MS)
			{
				if (!announced_connecting)
				{
					CAM_LOG_INFO("Connecting to the server {0}:{1}...", m_Config.ServerIP, m_Config.Port);
					announced_connecting = true;
				}

				DisplayConnectionStartMessage start = {};
				start.Header.Type = DISPLAY_CONNECTION_START;
				start.Header.Version = CAM_PROTOCOL_VERSION;
				memcpy(start.DisplayName, m_Config.Name.c_str(), std::min<size_t>(m_Config.Name.size(), MAX_FRAME_NAME_LENGTH - 1));
				memcpy(start.CameraFilter, m_Config.Camera.c_str(), std::min<size_t>(m_Config.Camera.size(), MAX_FRAME_NAME_LENGTH - 1));
				start.MaxFPS = m_Config.MaxFPS;
				m_Socket->Send(&start, sizeof(start), m_Host);
				last_request_ms = now;
			}
		}
		else if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)
		{
			ClientHeartbeatMessage heartbeat = {};
			heartbeat.Header.Type = CLIENT_HEARTBEAT;
			heartbeat.Header.Version = CAM_PROTOCOL_VERSION;
			m_Socket->Send(&heartbeat, sizeof(heartbeat), m_Host);
			last_heartbeat_ms = now;
		}

		// Process everything that has arrived.
		int received = 0;
		for (;;)
		{
			Core::addr_t addr = {};
			int32 len = m_Socket->Recv(BUF, sizeof(BUF), &addr);
			if (len <= 0)
			{
				break;
			}

			// ignore all messages from unknown senders
			if (addr.Value != m_Host.Value || len < (int32)sizeof(header_t))
			{
				continue;
			}

			header_t *header = (header_t *)BUF;
			if (header->Version != CAM_PROTOCOL_VERSION)
			{
				continue;
			}

			++received;
			last_receive_ms = Core::QueryMS();

			switch (header->Type)
			{
				case SERVER_DISPLAY_START:
				{
					if (len == sizeof(ServerDisplayStartResponse) && ((ServerDisplayStartResponse *)BUF)->Accepted && !m_Connected)
					{
						m_Connected = true;
						last_heartbeat_ms = last_receive_ms;
						CAM_LOG_INFO("Connected to the server.");
					}
					break;
				}

				case SERVER_CLIENT_UNKNOWN:
				{
					// The server forgot this display (timeout or restart). Register again.
					if (m_Connected)
					{
						CAM_LOG_INFO("The server does not know this display anymore, connecting again...");
						m_Connected = false;
						announced_connecting = true;
						last_request_ms = 0;
					}
					break;
				}

				case SERVER_FRAME_CHUNK:
				{
					// The server only sends frames to registered displays. If the response got lost, the frames prove it.
					if (!m_Connected)
					{
						m_Connected = true;
						last_heartbeat_ms = last_receive_ms;
						CAM_LOG_INFO("Connected to the server.");
					}

					OnFrameChunk(BUF, len);
					break;
				}
			}
		}

		if (received == 0)
		{
			Core::SleepMS(2);
		}
	}

	// Say goodbye, so the server frees this display right away (if the message gets lost, the server notices it by itself after its timeout).
	ClientConnectionCloseMessage close_message = {};
	close_message.Header.Type = CLIENT_CONNECTION_CLOSE;
	close_message.Header.Version = CAM_PROTOCOL_VERSION;
	m_Socket->Send(&close_message, sizeof(close_message), m_Host);
}

void DisplayClient::OnFrameChunk(Byte *message, int32 length)
{
	if (length < (int32)sizeof(ServerFrameChunkMessage))
	{
		return;
	}

	ServerFrameChunkMessage *chunk = (ServerFrameChunkMessage *)message;

	// Validate the chunk against its own header, nothing from the network is trusted.
	uint32 payload_size = (uint32)length - sizeof(ServerFrameChunkMessage);
	uint32 frame_size = chunk->FrameSize;
	uint32 chunk_count = chunk->ChunkCount;
	uint32 chunk_index = chunk->ChunkIndex;
	if (frame_size == 0 || frame_size > MAX_ENCODED_FRAME_SIZE ||
		chunk_count != (frame_size + FRAME_CHUNK_PAYLOAD_SIZE - 1) / FRAME_CHUNK_PAYLOAD_SIZE ||
		chunk_index >= chunk_count)
	{
		return;
	}

	uint32 offset = chunk_index * FRAME_CHUNK_PAYLOAD_SIZE;
	if (payload_size != std::min(FRAME_CHUNK_PAYLOAD_SIZE, frame_size - offset))
	{
		return;
	}

	char name[MAX_FRAME_NAME_LENGTH];
	memcpy(name, chunk->CameraName, MAX_FRAME_NAME_LENGTH);
	name[MAX_FRAME_NAME_LENGTH - 1] = 0;

	std::lock_guard<std::mutex> lock(m_FeedsMutex);
	CameraFeed &feed = m_Feeds[chunk->CameraId];
	feed.Name = name;

	FrameAssembly &assembly = feed.Assembly;
	if (assembly.Active && chunk->FrameId != assembly.FrameId)
	{
		// Wrap-around safe comparison. Chunks of older frames are late and useless.
		if ((int32)(chunk->FrameId - assembly.FrameId) < 0)
		{
			return;
		}

		// A newer frame started before the current one was complete, some datagram got lost. Drop the incomplete frame.
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
		return;
	}

	if (assembly.ReceivedMask[chunk_index])
	{
		// Duplicate datagram
		return;
	}

	memcpy(assembly.Data->data() + offset, message + sizeof(ServerFrameChunkMessage), payload_size);
	assembly.ReceivedMask[chunk_index] = true;
	++assembly.ReceivedChunks;

	if (assembly.ReceivedChunks == assembly.ChunkCount)
	{
		// The frame is complete. It is decoded by the thread, which shows the pictures, and only if it is not replaced by a newer one before.
		feed.Latest = assembly.Data;
		++feed.LatestNumber;
		feed.LastFrameMS = Core::QueryMS();

		assembly.Data.reset();
		assembly.Active = false;
	}
}

// Draws text on a dark background, so it is readable on every picture.
static void DrawLabel(cv::Mat &canvas, const std::string &text, cv::Point origin, double scale)
{
	int baseline = 0;
	cv::Size size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, 2, &baseline);
	cv::Rect background(origin.x, origin.y, size.width + 12, size.height + baseline + 10);
	background &= cv::Rect(0, 0, canvas.cols, canvas.rows);
	if (background.area() > 0)
	{
		cv::Mat part = canvas(background);
		part *= 0.35;
	}

	cv::putText(canvas, text, cv::Point(origin.x + 6, origin.y + size.height + 4), cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
}

// Draws text in the middle of the picture.
static void DrawCenteredText(cv::Mat &canvas, const std::string &text, double scale)
{
	int baseline = 0;
	cv::Size size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, 2, &baseline);
	cv::putText(canvas, text, cv::Point((canvas.cols - size.width) / 2, (canvas.rows + size.height) / 2), cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(200, 200, 200), 2, cv::LINE_AA);
}

cv::Mat DisplayClient::Compose(int width, int height, uint32 *fresh_cameras)
{
	cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(18, 18, 18));
	*fresh_cameras = 0;

	// Take over what the network thread received. Decoding takes long, so it happens after the lock is released.
	struct Pending
	{
		uint32 Id;
		std::string Name;
		EncodedFrame Frame;
		uint32 Number;
		int64 LastFrameMS;
	};

	std::vector<Pending> pending;
	int64 now = Core::QueryMS();
	{
		std::lock_guard<std::mutex> lock(m_FeedsMutex);
		for (auto it = m_Feeds.begin(); it != m_Feeds.end();)
		{
			// A camera, which is gone, disappears.
			if (it->second.LastFrameMS != 0 && now - it->second.LastFrameMS > REMOVE_CAMERA_AFTER_MS)
			{
				it = m_Feeds.erase(it);
				continue;
			}

			if (it->second.LastFrameMS != 0)
			{
				pending.push_back({ it->first, it->second.Name, it->second.Latest, it->second.LatestNumber, it->second.LastFrameMS });
			}

			++it;
		}
	}

	// Many cameras on one screen are small, so the pictures are decoded in half the size, which is much faster (important on a Raspberry Pi).
	int decode_flags = pending.size() > 1 ? cv::IMREAD_REDUCED_COLOR_2 : cv::IMREAD_COLOR;

	std::map<uint32, DecodedCamera> current;
	for (const Pending &entry : pending)
	{
		DecodedCamera &decoded = m_Decoded[entry.Id];
		if (decoded.Number != entry.Number || decoded.Image.empty())
		{
			cv::Mat image = entry.Frame ? cv::imdecode(*entry.Frame, decode_flags) : cv::Mat();
			if (!image.empty())
			{
				decoded.Image = image;
				decoded.Number = entry.Number;
			}
		}

		decoded.Name = entry.Name;
		decoded.LastFrameMS = entry.LastFrameMS;
		current[entry.Id] = decoded;
	}

	// Forget the pictures of cameras, which are gone.
	m_Decoded = current;

	if (!m_Connected && m_Decoded.empty())
	{
		DrawCenteredText(canvas, "Connecting to " + m_Config.ServerIP + ":" + std::to_string(m_Config.Port) + " ...", std::max(0.7, width / 1200.0));
		return canvas;
	}

	// The cameras are shown in the order of their names, so they do not jump around.
	std::vector<DecodedCamera *> cameras;
	for (auto &entry : m_Decoded)
	{
		if (!entry.second.Image.empty())
		{
			cameras.push_back(&entry.second);
		}
	}

	std::sort(cameras.begin(), cameras.end(), [](const DecodedCamera *a, const DecodedCamera *b) { return a->Name < b->Name; });

	if (cameras.empty())
	{
		std::string waiting = m_Config.Camera.empty() ? "Connected. Waiting for cameras ..." : "Connected. Waiting for the camera \"" + m_Config.Camera + "\" ...";
		DrawCenteredText(canvas, waiting, std::max(0.7, width / 1200.0));
		return canvas;
	}

	// A grid, which is as square as possible: 1 camera fills the screen, 2 are next to each other, 4 are 2 x 2, ...
	int count = (int)cameras.size();
	int columns = (int)std::ceil(std::sqrt((double)count));
	int rows = (count + columns - 1) / columns;
	double font_scale = std::max(0.5, std::min(width / columns, height / rows) / 700.0);

	for (int i = 0; i < count; ++i)
	{
		int column = i % columns;
		int row = i / columns;
		cv::Rect tile(column * width / columns, row * height / rows, (column + 1) * width / columns - column * width / columns, (row + 1) * height / rows - row * height / rows);

		const DecodedCamera &camera = *cameras[i];

		// The picture is scaled as large as possible, without changing its proportions (there are black bars, where it does not fit).
		double scale = std::min((double)tile.width / camera.Image.cols, (double)tile.height / camera.Image.rows);
		cv::Size size(std::max(1, (int)std::lround(camera.Image.cols * scale)), std::max(1, (int)std::lround(camera.Image.rows * scale)));
		cv::Rect target((tile.width - size.width) / 2 + tile.x, (tile.height - size.height) / 2 + tile.y, size.width, size.height);
		target &= cv::Rect(0, 0, width, height);
		if (target.area() <= 0)
		{
			continue;
		}

		cv::Mat part = canvas(target);
		cv::resize(camera.Image, part, target.size(), 0, 0, cv::INTER_LINEAR);

		bool stale = now - camera.LastFrameMS > NO_SIGNAL_AFTER_MS;
		if (stale)
		{
			part *= 0.3;
			cv::Mat tile_canvas = canvas(tile);
			DrawCenteredText(tile_canvas, "no signal", font_scale * 1.2);
		}
		else
		{
			++(*fresh_cameras);
		}

		DrawLabel(canvas, camera.Name, cv::Point(target.x + 8, target.y + 8), font_scale);
	}

	return canvas;
}
