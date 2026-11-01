#pragma once

#include <opencv2/opencv.hpp>

/// <summary>
/// Detects movement in a series of frames of a (fixed) camera. Every frame is compared with a picture of the background, which follows slow changes
/// (the daylight, a lamp turned on) but not things, that move. The pictures are scaled down and blurred first, so the noise of the camera is not
/// mistaken for movement. The server runs one for every camera, so the (weak) camera clients do not have to analyze anything.
/// </summary>
class MotionDetector
{
public:

	/// <param name="minAreaPercent">How much of the picture (in percent) has to change, to count as movement.</param>
	/// <param name="pixelThreshold">How much the brightness of a pixel (0 - 255) has to change, so that the pixel counts as changed.</param>
	/// <param name="lightShift">A change of the brightness of the whole picture (the median of all pixels, 0 - 255) by at least this much is a change of the
	/// lighting (a lamp turned on or off, the day/night mode of the camera), not a movement. 0 turns this check off.</param>
	/// <param name="lightAreaPercent">The same, if at least this part of the picture (in percent) changed at once. 0 turns this check off.</param>
	/// <param name="settleFrames">After a change of the lighting, no movement is reported for at least this many frames (the camera adjusts its exposure
	/// for a moment), while the background is learned again.</param>
	MotionDetector(double minAreaPercent = 1.0, int pixelThreshold = 25, double lightShift = 8.0, double lightAreaPercent = 50.0, int settleFrames = 15);

	/// <summary>
	/// Compares the frame with the background.
	/// </summary>
	/// <param name="frame">A frame of the camera (1, 3 or 4 channels).</param>
	/// <returns>Returns true, if something moves in the frame. The first frames only build the background and never report movement.</returns>
	bool Update(const cv::Mat &frame);

	/// <summary>
	/// The part of the picture (0 - 1), which changed in the last call of Update.
	/// </summary>
	double GetLastChangedArea() const { return m_LastChangedArea; }

	/// <summary>
	/// True, if the last call of Update saw a change of the lighting (and so reported no movement).
	/// </summary>
	bool IsLightingChange() const { return m_LightingChange; }

private:

	double m_MinArea;
	int m_PixelThreshold;
	double m_LightShift;
	double m_LightArea;
	int m_SettleLength;
	int m_SettleFrames = 0;
	bool m_LightingChange = false;
	double m_LastChangedArea = 0.0;
	cv::Mat m_Background;
	int m_WarmupFrames = 5;
};
