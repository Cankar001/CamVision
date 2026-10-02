#pragma once

#include "Features.h"

#if FRAME_ANALYSIS // FRAME ANALYSIS (movement and face detection in the camera client): switch all blocks with this tag to "#if 1" to enable it

#include <opencv2/opencv.hpp>

/// <summary>
/// Detects movement in a series of frames of a (fixed) camera. Every frame is compared with a picture of the background, which follows slow changes
/// (the daylight, a lamp turned on) but not things, that move. The pictures are scaled down and blurred first, so the noise of the camera is not
/// mistaken for movement.
/// </summary>
class MotionDetector
{
public:

	/// <param name="minAreaPercent">How much of the picture (in percent) has to change, to count as movement.</param>
	/// <param name="pixelThreshold">How much the brightness of a pixel (0 - 255) has to change, so that the pixel counts as changed.</param>
	MotionDetector(double minAreaPercent = 1.0, int pixelThreshold = 25);

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

private:

	double m_MinArea;
	int m_PixelThreshold;
	double m_LastChangedArea = 0.0;
	cv::Mat m_Background;
	int m_WarmupFrames = 5;
};

#endif // FRAME_ANALYSIS
