#pragma once

#include <Cam-Core.h>

#include <opencv2/opencv.hpp>

class Camera
{
public:

	Camera(bool flipImage, uint32 width = 0, uint32 height = 0);
	~Camera();

	/// <summary>
	/// Tries to re-init the camera stream.
	/// </summary>
	void Invalidate();

	/// <summary>
	/// Frees all camera resources.
	/// </summary>
	void Release();

	/// <summary>
	/// Generates the most current frame. Must be called in a loop, to generate continuesly new images.
	/// </summary>
	void GenerateFrames();

	/// <summary>
	/// Gets a specific frame, which was captured before by GenerateFrames. Must be called in a loop, to retrieve a specific frame continuesly.
	/// </summary>
	/// <param name="frameIndex">The frameIndex, from which the image should be extracted.</param>
	/// <param name="out_frame_size">The total size of the frame data in bytes.</param>
	/// <param name="out_frame_width">The width of the resulting frame data.</param>
	/// <param name="out_frame_height">The height of the resulting frame data.</param>
	/// <returns>Returns the pixel data of the requested frame number if successful, otherwise nullptr. The caller is responsible for deleting the buffer when not used anymore.</returns>
	Byte *GetFrame(uint32 frameIndex, uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height);

	/// <summary>
	/// Gets every current frame, must be called continuesly, to retrieve each current frame (aka. live video feed)
	/// </summary>
	/// <param name="out_frame_size">The total size of the frame data in bytes.</param>
	/// <param name="out_frame_width">The width of the resulting frame data.</param>
	/// <param name="out_frame_height">The height of the resulting frame data.</param>
	/// <returns>Returns the pixel data of the most current frame if successful, otherwise nullptr. The caller is responsible for deleting the buffer when not used anymore.</returns>
	Byte *GetCurrentFrame(uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height);

	/// <summary>
	/// Shows a specific frame, which was captured before by GenerateFrames. Must be called in a loop, to display a specific frame continuesly.
	/// </summary>
	/// <param name="frameIndex">The frameIndex, from which the image should be extracted.</param>
	/// <param name="out_frame_size">The total size of the frame data in bytes.</param>
	/// <param name="out_frame_width">The width of the resulting frame data.</param>
	/// <param name="out_frame_height">The height of the resulting frame data.</param>
	/// <returns>Returns the pixel data of the requested frame number if successful, otherwise nullptr. The caller is responsible for deleting the buffer when not used anymore.</returns>
	Byte *Show(uint32 frameIndex, uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height);

	/// <summary>
	/// Shows every current frame, must be called continuesly, to display each current frame (aka. live video feed)
	/// </summary>
	/// <param name="out_frame_size">The total size of the frame data in bytes.</param>
	/// <param name="out_frame_width">The width of the resulting frame data.</param>
	/// <param name="out_frame_height">The height of the resulting frame data.</param>
	/// <returns>Returns the pixel data of the most current frame if successful, otherwise nullptr. The caller is responsible for deleting the buffer when not used anymore.</returns>
	Byte *ShowLive(uint32 *out_frame_size, uint32 *out_frame_width, uint32 *out_frame_height);

	/// <summary>
	/// Determines, if the camera was closed by the user or is currently running.
	/// </summary>
	/// <returns>Returns true, if the camera is currently running. If it has been closed, it returns false.</returns>
	bool IsRunning() const { return m_CameraRunning; }

	/// <summary>
	/// Returns the current number of frames, which are read from the camera feed.
	/// </summary>
	/// <returns>Returns the current frame count.</returns>
	uint32 GetFrameCount() const { return m_FrameCount; }

	/// <summary>
	/// Returns the current color format, gets overwritten everytime GenerateFrames() is called.
	/// </summary>
	/// <returns>Returns the current color format</returns>
	uint32 GetFormat() const { return m_Format; }

private:

	/// <summary>
	/// Opens the camera stream and applies resolution and format.
	/// </summary>
	void OpenStream();

	/// <summary>
	/// Tries to open the camera with one specific backend and verifies, that frames can be read.
	/// </summary>
	bool TryOpenStream(int32 backend, bool useMjpg);

	/// <summary>
	/// Zooms in- or out in the current frame, calculates the view area based on the center parameter. 
	/// Copies the Mat object into a new one, without modifying the parameter.
	/// </summary>
	/// <param name="frame">The frame to be used for the zoom.</param>
	/// <param name="center">The position, where the frame should be zoomed to.</param>
	/// <returns>Returns a new, zoomed copy of the frame.</returns>
	cv::Mat Zoom(cv::Mat frame, std::pair<float, float> center);

	/// <summary>
	/// Sets the zoom parameter to be zoomed in for the next call of Zoom.
	/// </summary>
	void ZoomIn();

	/// <summary>
	/// Sets the zoom parameter to be zoomed out for the next call of Zoom.
	/// </summary>
	void ZoomOut();

private:

	uint32 m_Width = 0;
	uint32 m_Height = 0;
	uint32 m_FrameCount = 0;
	int32 m_Format = 0;
	bool m_FlipImage = false;
	float m_Scale = 1.0f;

	bool m_CameraRunning = true;

	bool m_TouchedZoom = false;
	float m_CenterX = 0, m_CenterY = 0;
	float m_RadiusX = 0, m_RadiusY = 0;

	cv::VideoCapture m_CameraStream;
	Core::ThreadSafeQueue<cv::Mat> m_ImageQueue;
};

