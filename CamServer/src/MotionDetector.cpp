#include "MotionDetector.h"

#include <algorithm>
#include <cmath>
#include <vector>

MotionDetector::MotionDetector(double minAreaPercent, int pixelThreshold, double lightShift, double lightAreaPercent, int settleFrames)
	: m_MinArea(minAreaPercent / 100.0), m_PixelThreshold(pixelThreshold), m_LightShift(lightShift), m_LightArea(lightAreaPercent / 100.0), m_SettleLength(std::max(settleFrames, 0))
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
		m_SettleFrames = 0;
		m_LightingChange = false;
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

	// A change of the lighting (a lamp, which turns on in front of the camera, the day/night mode) changes the whole picture at once. A person does not do
	// that: the median of all pixels stays where it was, as long as the person covers less than half of the picture.
	cv::Mat signed_difference;
	cv::subtract(gray, background8, signed_difference, cv::noArray(), CV_16S);
	std::vector<short> values(signed_difference.begin<short>(), signed_difference.end<short>());
	std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
	double shift = values.empty() ? 0.0 : (double)values[values.size() / 2];

	m_LightingChange = (m_LightShift > 0.0 && std::abs(shift) >= m_LightShift) || (m_LightArea > 0.0 && m_LastChangedArea >= m_LightArea);
	if (m_LightingChange)
	{
		// Learn the new lighting at once (the first frames after it are the ones, where the camera is still adjusting, so this goes on as long as the
		// picture changes), and wait until it is calm.
		m_SettleFrames = m_SettleLength;
		cv::accumulateWeighted(gray, m_Background, 0.5);
		return false;
	}

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

	// Still settling after a change of the lighting: the background follows as usual, but nothing is reported yet.
	if (m_SettleFrames > 0)
	{
		--m_SettleFrames;
		return false;
	}

	return m_LastChangedArea >= m_MinArea;
}
