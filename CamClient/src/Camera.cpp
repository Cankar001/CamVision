#include "Camera.h"

#include <thread>

#include "Core/Log.h"

#define MAX_RETRIES 5

// Frames to discard after opening the camera, cameras deliver grey/dark frames while auto exposure and white balance settle.
#define CAMERA_WARMUP_FRAMES 30

bool Camera::TryOpenStream(int32 backend, bool useMjpg)
{
	m_CameraStream.release();
	m_CameraStream.open(m_Index, backend);
	if (!m_CameraStream.isOpened())
	{
		CAM_LOG_ERROR("Camera {0}: could not be opened with backend {1}.", m_Index, backend);
		return false;
	}

	if (useMjpg)
	{
		// Request MJPG, uncompressed formats are limited by USB bandwidth.
		m_CameraStream.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
	}

	if (m_Width != 0 && m_Height != 0)
	{
		m_CameraStream.set(cv::CAP_PROP_FRAME_WIDTH, (double)m_Width);
		m_CameraStream.set(cv::CAP_PROP_FRAME_HEIGHT, (double)m_Height);
	}

	// Cameras deliver grey/dark frames while auto exposure and white balance settle, so discard the first frames.
	// The last one has to be valid, otherwise this combination of backend and format does not work.
	cv::Mat frame;
	bool got_frame = false;
	for (uint32 i = 0; i < CAMERA_WARMUP_FRAMES; ++i)
	{
		got_frame = m_CameraStream.read(frame) && !frame.empty();
	}

	if (!got_frame)
	{
		CAM_LOG_ERROR("Camera {0}: opened with backend {1} (MJPG: {2}), but no frames could be read.", m_Index, backend, useMjpg);
		return false;
	}

	m_Width = (uint32)frame.cols;
	m_Height = (uint32)frame.rows;
	CAM_LOG_INFO("Camera {0}: using backend {1} (MJPG: {2}), {3}x{4}.", m_Index, m_CameraStream.getBackendName(), useMjpg, m_Width, m_Height);
	return true;
}

void Camera::OpenStream()
{
	bool opened = false;

#ifdef CAM_PLATFORM_WINDOWS
	opened = TryOpenStream(cv::CAP_DSHOW, true) || TryOpenStream(cv::CAP_DSHOW, false) || TryOpenStream(cv::CAP_MSMF, false);
#else
	// Video4Linux is the native camera API on Linux (USB cameras, Raspberry Pi), fall back to whatever OpenCV finds.
	opened = TryOpenStream(cv::CAP_V4L2, true) || TryOpenStream(cv::CAP_V4L2, false) || TryOpenStream(cv::CAP_ANY, false);
#endif

	if (!opened)
	{
		CAM_LOG_ERROR("Camera {} could not be started with any backend!", m_Index);
		m_CameraStream.release();
		m_CameraRunning = false;
		return;
	}

	m_CenterX = (float)m_Width / 2;
	m_CenterY = (float)m_Height / 2;
	m_Format = (int32)m_CameraStream.get(cv::CAP_PROP_FORMAT);
}

Camera::Camera(const CameraConfig &config)
	: m_FlipImage(config.FlipImage), m_Index(config.Index), m_Width(config.Width), m_Height(config.Height)
{
	m_Fullscreen = config.Fullscreen;
	OpenStream();
}

void Camera::EnsureWindow()
{
	if (m_WindowCreated)
	{
		return;
	}

	if (m_Fullscreen)
	{
		// Like the display application: the whole screen, without a border or a title bar.
		cv::namedWindow("Frame", cv::WINDOW_NORMAL);
		cv::setWindowProperty("Frame", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
	}
	else
	{
		// Resizable window, which keeps the aspect ratio of the frame instead of stretching it.
		cv::namedWindow("Frame", cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
	}

	m_WindowCreated = true;
}

void Camera::HandleKey(int key)
{
	// On some systems waitKey returns more than the key code in the higher bits.
	key &= 0xFF;
	if (key == 27 || key == 'q')
	{
		Release();
	}
	else if (key == 'z')
	{
		ZoomIn();
	}
	else if (key == 'x')
	{
		ZoomOut();
	}
}

Camera::~Camera()
{
	Release();
}

void Camera::Invalidate()
{
	Release();

	m_CameraRunning = true;
	OpenStream();
}


void Camera::GenerateFrames()
{
	cv::Mat frame;
	static uint32 failed_retries = 0;
	static uint32 invalidate_count = 0;

	bool success = m_CameraStream.read(frame);
	if (!success)
	{
		++failed_retries;
		if (failed_retries >= MAX_RETRIES)
		{
			if (invalidate_count >= MAX_RETRIES)
			{
				Release();
				return;
			}

			Invalidate();
			++invalidate_count;
		}
		
		return;
	}

	m_Format = frame.type();
	
	if (m_FlipImage)
	{
		cv::flip(frame, frame, 0);
		cv::flip(frame, frame, 1);
	}

	if (m_TouchedZoom)
	{
		frame = Zoom(frame, { m_CenterX, m_CenterY });
	}
	else
	{
		if (m_Scale != 1)
			frame = Zoom(frame, { 0.0f, 0.0f });
	}

	m_ImageQueue.Enqueue(frame);
	++m_FrameCount;
}

Byte *Camera::GetFrame(uint32 frameIndex, uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height)
{
	if (!out_frame_size || !out_frame_width || !out_frame_height)
	{
		return nullptr;
	}

	if (frameIndex >= m_ImageQueue.Size())
	{
		*out_frame_size = 0;
		*out_frame_width = 0;
		*out_frame_height = 0;
		return nullptr;
	}

	cv::Mat frame = m_ImageQueue.Get(frameIndex);

	*out_frame_size = (uint32)(frame.total() * frame.elemSize());
	*out_frame_width = frame.cols;
	*out_frame_height = frame.rows;

	Byte *frame_data = new Byte[*out_frame_size];
	memcpy(frame_data, frame.data, *out_frame_size);
	return frame_data;
}

Byte *Camera::GetCurrentFrame(uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height)
{
	if (!out_frame_size || !out_frame_width || !out_frame_height)
	{
		return nullptr;
	}

	// For a live feed only the newest frame matters, skip everything that piled up while the previous frame was processed.
	while (m_ImageQueue.Size() > 1)
	{
		m_ImageQueue.Dequeue();
	}

	// Don't block forever, the camera might have been stopped in the meantime.
	cv::Mat frame;
	if (!m_ImageQueue.TryDequeue(frame, 100))
	{
		*out_frame_size = 0;
		*out_frame_width = 0;
		*out_frame_height = 0;
		return nullptr;
	}

	*out_frame_size = (uint32)(frame.total() * frame.elemSize());
	*out_frame_width = frame.cols;
	*out_frame_height = frame.rows;

	Byte *frame_data = new Byte[*out_frame_size];
	memcpy(frame_data, frame.data, *out_frame_size);
	return frame_data;
}

void Camera::Release()
{
	m_CameraRunning = false;
	m_CameraStream.release();

	// Headless clients (no display) must not touch the window system.
	if (m_WindowCreated)
	{
		cv::destroyAllWindows();
		m_WindowCreated = false;
	}
}

Byte *Camera::Show(uint32 frameIndex, uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height)
{
	if (!out_frame_size || !out_frame_width || !out_frame_height)
	{
		return nullptr;
	}

	if (frameIndex >= m_ImageQueue.Size())
	{
		*out_frame_size = 0;
		*out_frame_width = 0;
		*out_frame_height = 0;
		return nullptr;
	}

	cv::Mat frame = m_ImageQueue.Get(frameIndex);
	EnsureWindow();
	cv::imshow("Frame", frame);

	*out_frame_size = (uint32)(frame.total() * frame.elemSize());
	*out_frame_width = frame.cols;
	*out_frame_height = frame.rows;

	HandleKey(cv::waitKey(1));

	Byte *frame_data = new Byte[*out_frame_size];
	memcpy(frame_data, frame.data, *out_frame_size);
	return frame_data;
}

Byte *Camera::ShowLive(uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height)
{
	if (!out_frame_size || !out_frame_width || !out_frame_height)
	{
		return nullptr;
	}

	// For a live feed only the newest frame matters, skip everything that piled up while the previous frame was processed.
	while (m_ImageQueue.Size() > 1)
	{
		m_ImageQueue.Dequeue();
	}

	cv::Mat frame;
	if (!m_ImageQueue.TryDequeue(frame, 100))
	{
		*out_frame_size = 0;
		*out_frame_width = 0;
		*out_frame_height = 0;
		return nullptr;
	}

	*out_frame_size = (uint32)(frame.total() * frame.elemSize());
	*out_frame_width = frame.cols;
	*out_frame_height = frame.rows;

	EnsureWindow();
	cv::imshow("Frame", frame);

	HandleKey(cv::waitKey(1));

	Byte *frame_data = new Byte[*out_frame_size];
	memcpy(frame_data, frame.data, *out_frame_size);
	return frame_data;
}

cv::Mat Camera::Zoom(cv::Mat frame, std::pair<float, float> center)
{
	int32 width = frame.cols;
	int32 height = frame.rows;

	if (center.first == 0 && center.second == 0)
	{
		m_CenterX = (float)width / 2.0f;
		m_CenterY = (float)height / 2.0f;
		m_RadiusX = (float)width / 2.0f;
		m_RadiusY = (float)height / 2.0f;
	}
	else
	{
		uint32 rate = width / height;
		m_CenterX = center.first;
		m_CenterY = center.second;

		if (m_CenterX < width * (1 - rate))
		{
			m_CenterX = (float)(width * (1 - rate));
		}
		else if (m_CenterX > width * rate)
		{
			m_CenterX = (float)(width * rate);
		}

		if (m_CenterY < height * (1 - rate))
		{
			m_CenterY = (float)(height * (1 - rate));
		}
		else if (m_CenterY > height * rate)
		{
			m_CenterY = (float)(height * rate);
		}

		float left_x = m_CenterX;
		float right_x = width - m_CenterX;
		float up_y = height - m_CenterY;
		float down_y = m_CenterY;

		m_RadiusX = MIN(left_x, right_x);
		m_RadiusY = MIN(up_y, down_y);
	}

	m_RadiusX = m_Scale * m_RadiusX;
	m_RadiusY = m_Scale * m_RadiusY;

	float min_x = m_CenterX - m_RadiusX;
	float max_x = m_CenterX + m_RadiusX;
	float min_y = m_CenterY - m_RadiusY;
	float max_y = m_CenterY + m_RadiusY;

	cv::Rect rect((int32)min_x, (int32)min_y, (int32)max_x, (int32)max_y);
	return frame(rect);
}

void Camera::ZoomIn()
{
	if (m_Scale > 0.2f)
		m_Scale -= 0.1f;
}

void Camera::ZoomOut()
{
	if (m_Scale < 1.0f)
		m_Scale += 0.1f;

	if (m_Scale == 1.0f)
	{
		m_CenterX = (float)m_Width;
		m_CenterY = (float)m_Height;
		m_TouchedZoom = false;
	}
}

