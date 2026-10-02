#include "MotionDetector.h"

#if FRAME_ANALYSIS // FRAME ANALYSIS (movement and face detection in the camera client): switch all blocks with this tag to "#if 1" to enable it

#include <algorithm>

MotionDetector::MotionDetector(double minAreaPercent, int pixelThreshold)
	: m_MinArea(minAreaPercent / 100.0), m_PixelThreshold(pixelThreshold)
{
}

bool MotionDetector::Update(const cv::Mat &frame)
{
	if (frame.empty())
	{
		return false;
	}

	// Small and blurred: faster, and the noise of the camera disappears.
	cv::Mat small = frame;
	if (frame.cols > 320)
	{
		cv::resize(frame, small, cv::Size(320, std::max(1, frame.rows * 320 / frame.cols)), 0, 0, cv::INTER_AREA);
	}

	cv::Mat gray;
	if (small.channels() == 3)
	{
		cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);
	}
	else if (small.channels() == 4)
	{
		cv::cvtColor(small, gray, cv::COLOR_BGRA2GRAY);
	}
	else
	{
		gray = small.clone();
	}

	cv::GaussianBlur(gray, gray, cv::Size(15, 15), 0);

	// A new size (for example after the camera was restarted) starts again.
	if (m_Background.empty() || m_Background.size() != gray.size())
	{
		gray.convertTo(m_Background, CV_32F);
		m_WarmupFrames = 5;
		m_LastChangedArea = 0.0;
		return false;
	}

	cv::Mat background8;
	m_Background.convertTo(background8, CV_8U);

	cv::Mat difference, changed;
	cv::absdiff(gray, background8, difference);
	cv::threshold(difference, changed, m_PixelThreshold, 255, cv::THRESH_BINARY);
	cv::dilate(changed, changed, cv::Mat(), cv::Point(-1, -1), 2);

	m_LastChangedArea = (double)cv::countNonZero(changed) / (double)changed.total();

	// The background follows the picture, but not where something moves right now (otherwise a person, who stands still, would become background).
	// During the first frames everything is learned quickly.
	cv::Mat unchanged;
	cv::bitwise_not(changed, unchanged);
	double alpha = m_WarmupFrames > 0 ? 0.5 : 0.05;
	cv::accumulateWeighted(gray, m_Background, alpha, unchanged);

	// Very slowly everything is learned, also what moves: an object, which was left behind, becomes background after a while (otherwise it would count as
	// movement forever).
	cv::accumulateWeighted(gray, m_Background, 0.005);

	if (m_WarmupFrames > 0)
	{
		--m_WarmupFrames;
		return false;
	}

	return m_LastChangedArea >= m_MinArea;
}

#endif // FRAME_ANALYSIS
