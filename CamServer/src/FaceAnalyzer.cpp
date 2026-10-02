#include "FaceAnalyzer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>

#include "Core/Log.h"

// cv::FaceDetectorYN and cv::FaceRecognizerSF exist since OpenCV 4.5.4. On older versions (for example Debian 11 or Ubuntu 20.04) the face
// analysis is not available, but everything else still builds.
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && (CV_VERSION_MINOR > 5 || (CV_VERSION_MINOR == 5 && CV_VERSION_REVISION >= 4)))
#define CAM_HAS_FACE_API 1
#endif

static const char *DETECTOR_URL = "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx";
static const char *RECOGNIZER_URL = "https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx";

struct FaceAnalyzer::Impl
{
#ifdef CAM_HAS_FACE_API
	cv::Ptr<cv::FaceDetectorYN> Detector;
	cv::Ptr<cv::FaceRecognizerSF> Recognizer;
#endif

	// The face features of all photos of every known person.
	std::map<std::string, std::vector<cv::Mat>> Known;
	std::string KnownFingerprint;
	int64 LastKnownCheckMS = 0;

	// Read by other threads (the preview), while the analysis thread may load the known faces again.
	std::atomic<bool> CanRecognizeFlag{ false };
};

static bool IsImageFile(const std::filesystem::path &path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".bmp";
}

// Lists all photos in the known faces folder together with the name of the person, who is on it.
static std::vector<std::pair<std::string, std::filesystem::path>> ListKnownPhotos(const std::string &folder)
{
	std::vector<std::pair<std::string, std::filesystem::path>> photos;

	std::error_code error;
	std::filesystem::directory_iterator it(folder, error);
	if (error)
	{
		return photos;
	}

	for (; !error && it != std::filesystem::directory_iterator(); it.increment(error))
	{
		std::error_code entry_error;
		if (it->is_directory(entry_error))
		{
			// folder name = person
			std::string person = it->path().filename().string();
			std::filesystem::directory_iterator inner(it->path(), entry_error);
			for (; !entry_error && inner != std::filesystem::directory_iterator(); inner.increment(entry_error))
			{
				if (inner->is_regular_file() && IsImageFile(inner->path()))
				{
					photos.push_back({ person, inner->path() });
				}
			}
		}
		else if (it->is_regular_file(entry_error) && IsImageFile(it->path()))
		{
			// file name = person
			photos.push_back({ it->path().stem().string(), it->path() });
		}
	}

	std::sort(photos.begin(), photos.end());
	return photos;
}

static std::string FingerprintOf(const std::vector<std::pair<std::string, std::filesystem::path>> &photos)
{
	std::string fingerprint;
	for (const auto &photo : photos)
	{
		std::error_code size_error, time_error;
		uint64 size = (uint64)std::filesystem::file_size(photo.second, size_error);
		auto time = std::filesystem::last_write_time(photo.second, time_error);
		fingerprint += photo.second.string() + "|" + std::to_string(size) + "|" + std::to_string((int64)time.time_since_epoch().count()) + "\n";
	}

	return fingerprint;
}

FaceAnalyzer::FaceAnalyzer(const FaceConfig &config)
	: m_Impl(new Impl()), m_Config(config)
{
}

FaceAnalyzer::~FaceAnalyzer()
{
}

bool FaceAnalyzer::Initialize()
{
#ifndef CAM_HAS_FACE_API
	CAM_LOG_ERROR("The face detection needs OpenCV 4.5.4 or newer, but this program was built with OpenCV {}.", CV_VERSION);
	return false;
#else
	if (!std::filesystem::exists(m_Config.DetectorModel))
	{
		CAM_LOG_ERROR("The face detector model {0} does not exist. Download it from {1} and store it there.", m_Config.DetectorModel, DETECTOR_URL);
		return false;
	}

	try
	{
		m_Impl->Detector = cv::FaceDetectorYN::create(m_Config.DetectorModel, "", cv::Size(320, 320), m_Config.ScoreThreshold, 0.3f, 5000);
	}
	catch (const cv::Exception &e)
	{
		CAM_LOG_ERROR("Could not load the face detector model {0}: {1}", m_Config.DetectorModel, e.what());
		return false;
	}

	if (!std::filesystem::exists(m_Config.RecognizerModel))
	{
		CAM_LOG_WARN("The face recognizer model {0} does not exist, faces are only detected, not recognized. Download it from {1} to recognize known people.", m_Config.RecognizerModel, RECOGNIZER_URL);
	}
	else
	{
		try
		{
			m_Impl->Recognizer = cv::FaceRecognizerSF::create(m_Config.RecognizerModel, "");
		}
		catch (const cv::Exception &e)
		{
			CAM_LOG_ERROR("Could not load the face recognizer model {0}: {1}. Faces are only detected, not recognized.", m_Config.RecognizerModel, e.what());
		}
	}

	if (m_Impl->Recognizer)
	{
		LoadKnownFaces();
	}

	return true;
#endif
}

void FaceAnalyzer::LoadKnownFaces()
{
#ifdef CAM_HAS_FACE_API
	auto photos = ListKnownPhotos(m_Config.KnownFacesPath);

	// Keep the old faces until the new ones are loaded, the fingerprint is stored, so a broken photo is not loaded again and again.
	std::map<std::string, std::vector<cv::Mat>> known;
	uint32 without_face = 0;
	for (const auto &photo : photos)
	{
		cv::Mat image = cv::imread(photo.second.string(), cv::IMREAD_COLOR);
		if (image.empty())
		{
			CAM_LOG_WARN("Could not read the photo {}.", photo.second.string());
			++without_face;
			continue;
		}

		try
		{
			// Detect on the original size of the photo, scaled down if it is large (like for all frames).
			double scale = 1.0;
			cv::Mat small = image;
			if (m_Config.DetectWidth > 0 && image.cols > m_Config.DetectWidth)
			{
				scale = (double)image.cols / m_Config.DetectWidth;
				cv::resize(image, small, cv::Size(m_Config.DetectWidth, std::max(1, (int)std::lround(image.rows / scale))));
			}

			m_Impl->Detector->setInputSize(small.size());
			cv::Mat faces;
			m_Impl->Detector->detect(small, faces);
			if (faces.empty() || faces.rows < 1)
			{
				CAM_LOG_WARN("No face found in the photo {}, it is not used.", photo.second.string());
				++without_face;
				continue;
			}

			// The photo should show one person. If there are more faces, the biggest one is used.
			int best = 0;
			float best_area = 0.0f;
			for (int i = 0; i < faces.rows; ++i)
			{
				float area = faces.at<float>(i, 2) * faces.at<float>(i, 3);
				if (area > best_area)
				{
					best_area = area;
					best = i;
				}
			}

			cv::Mat face = faces.row(best).clone();
			for (int c = 0; c < 14; ++c)
			{
				face.at<float>(0, c) = (float)(face.at<float>(0, c) * scale);
			}

			cv::Mat aligned, feature;
			m_Impl->Recognizer->alignCrop(image, face, aligned);
			m_Impl->Recognizer->feature(aligned, feature);
			known[photo.first].push_back(feature.clone());
		}
		catch (const cv::Exception &e)
		{
			CAM_LOG_WARN("Could not use the photo {0}: {1}", photo.second.string(), e.what());
			++without_face;
		}
	}

	uint32 used = 0;
	for (const auto &person : known)
	{
		used += (uint32)person.second.size();
	}

	m_Impl->Known = std::move(known);
	m_Impl->KnownFingerprint = FingerprintOf(photos);
	m_Impl->CanRecognizeFlag = m_Impl->Recognizer && !m_Impl->Known.empty();

	if (photos.empty())
	{
		CAM_LOG_INFO("No known faces in {}. Faces are detected, but nobody can be recognized. Add one folder per person with photos.", m_Config.KnownFacesPath);
	}
	else
	{
		CAM_LOG_INFO("Known faces: {0} people from {1} photos ({2} photos without a usable face).", m_Impl->Known.size(), used, without_face);
	}
#endif
}

void FaceAnalyzer::ReloadKnownFacesIfChanged()
{
#ifdef CAM_HAS_FACE_API
	if (!m_Impl->Recognizer)
	{
		return;
	}

	// Looking at the folder is cheap, but not needed more than every few seconds.
	int64 now = Core::QueryMS();
	if (now - m_Impl->LastKnownCheckMS < 5000)
	{
		return;
	}

	m_Impl->LastKnownCheckMS = now;
	if (FingerprintOf(ListKnownPhotos(m_Config.KnownFacesPath)) != m_Impl->KnownFingerprint)
	{
		CAM_LOG_INFO("The photos in {} changed, loading the known faces again...", m_Config.KnownFacesPath);
		LoadKnownFaces();
	}
#endif
}

bool FaceAnalyzer::CanRecognize() const
{
	return m_Impl->CanRecognizeFlag;
}

uint32 FaceAnalyzer::GetKnownPeopleCount() const
{
	return (uint32)m_Impl->Known.size();
}

std::vector<FaceResult> FaceAnalyzer::Analyze(const cv::Mat &frame)
{
	std::vector<FaceResult> results;

#ifdef CAM_HAS_FACE_API
	if (frame.empty() || !m_Impl->Detector)
	{
		return results;
	}

	// The detection is done on a smaller frame, which is much faster. All coordinates are scaled back afterwards.
	double scale = 1.0;
	cv::Mat small = frame;
	if (m_Config.DetectWidth > 0 && frame.cols > m_Config.DetectWidth)
	{
		scale = (double)frame.cols / m_Config.DetectWidth;
		cv::resize(frame, small, cv::Size(m_Config.DetectWidth, std::max(1, (int)std::lround(frame.rows / scale))));
	}

	m_Impl->Detector->setInputSize(small.size());
	cv::Mat faces;
	m_Impl->Detector->detect(small, faces);
	if (faces.empty())
	{
		return results;
	}

	bool recognize = m_Impl->Recognizer && !m_Impl->Known.empty();
	for (int i = 0; i < faces.rows; ++i)
	{
		// Box and landmarks (the first 14 values) in the coordinates of the original frame.
		cv::Mat face = faces.row(i).clone();
		for (int c = 0; c < 14; ++c)
		{
			face.at<float>(0, c) = (float)(face.at<float>(0, c) * scale);
		}

		FaceResult result;
		result.Box = cv::Rect((int)face.at<float>(0, 0), (int)face.at<float>(0, 1), (int)face.at<float>(0, 2), (int)face.at<float>(0, 3)) & cv::Rect(0, 0, frame.cols, frame.rows);
		result.Score = face.at<float>(0, 14);
		if (result.Box.area() <= 0)
		{
			continue;
		}

		if (recognize)
		{
			try
			{
				cv::Mat aligned, feature;
				m_Impl->Recognizer->alignCrop(frame, face, aligned);
				m_Impl->Recognizer->feature(aligned, feature);
				feature = feature.clone();

				double best = -1.0;
				const std::string *best_name = nullptr;
				for (const auto &person : m_Impl->Known)
				{
					for (const cv::Mat &known_feature : person.second)
					{
						double similarity = m_Impl->Recognizer->match(feature, known_feature, cv::FaceRecognizerSF::FR_COSINE);
						if (similarity > best)
						{
							best = similarity;
							best_name = &person.first;
						}
					}
				}

				result.Similarity = (float)best;
				if (best_name && best >= m_Config.MatchThreshold)
				{
					result.Recognized = true;
					result.Name = *best_name;
				}
			}
			catch (const cv::Exception &e)
			{
				CAM_LOG_WARN("Could not recognize a face: {}", e.what());
			}
		}

		results.push_back(result);
	}
#endif

	return results;
}

void DrawFaces(cv::Mat &frame, const std::vector<FaceResult> &faces, bool recognitionActive)
{
	for (const FaceResult &face : faces)
	{
		cv::Scalar color;
		std::string label;
		if (!recognitionActive)
		{
			color = cv::Scalar(0, 220, 255);	// yellow (BGR): a face, nobody to compare with
			label = "face";
		}
		else if (face.Recognized)
		{
			color = cv::Scalar(0, 200, 0);	// green
			char text[32];
			snprintf(text, sizeof(text), " %.2f", face.Similarity);
			label = face.Name + text;
		}
		else
		{
			color = cv::Scalar(0, 0, 255);	// red
			label = "unknown";
		}

		int thickness = std::max(2, frame.cols / 400);
		cv::rectangle(frame, face.Box, color, thickness);

		double font_scale = std::max(0.5, frame.cols / 1200.0);
		int baseline = 0;
		cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, font_scale, 2, &baseline);
		int y = std::max(face.Box.y - 6, text_size.height + 4);
		cv::rectangle(frame, cv::Rect(face.Box.x, y - text_size.height - 4, text_size.width + 6, text_size.height + 8), color, cv::FILLED);
		cv::putText(frame, label, cv::Point(face.Box.x + 3, y), cv::FONT_HERSHEY_SIMPLEX, font_scale, cv::Scalar(0, 0, 0), 2);
	}
}

int RunFaceTest(const FaceConfig &config, const std::string &imagePath)
{
	cv::Mat image = cv::imread(imagePath, cv::IMREAD_COLOR);
	if (image.empty())
	{
		std::cerr << "Could not read the image " << imagePath << std::endl;
		return 1;
	}

	FaceAnalyzer analyzer(config);
	if (!analyzer.Initialize())
	{
		std::cerr << "The face analysis could not be started, see the messages above." << std::endl;
		return 1;
	}

	std::cout << "Known people: " << analyzer.GetKnownPeopleCount() << (analyzer.CanRecognize() ? "" : "  (recognition is not active)") << std::endl;

	std::vector<FaceResult> faces = analyzer.Analyze(image);
	std::cout << "Faces found in " << imagePath << ": " << faces.size() << std::endl;
	for (size_t i = 0; i < faces.size(); ++i)
	{
		const FaceResult &face = faces[i];
		std::cout << "  Face " << (i + 1) << ": position " << face.Box.x << "," << face.Box.y << " size " << face.Box.width << "x" << face.Box.height << ", detector score " << face.Score;
		if (analyzer.CanRecognize())
		{
			if (face.Recognized)
			{
				std::cout << " -> " << face.Name << " (similarity " << face.Similarity << ")";
			}
			else
			{
				std::cout << " -> unknown (best similarity " << face.Similarity << ", needed " << config.MatchThreshold << ")";
			}
		}

		std::cout << std::endl;
	}

	DrawFaces(image, faces, analyzer.CanRecognize());
	std::string output = imagePath + ".faces.jpg";
	if (cv::imwrite(output, image))
	{
		std::cout << "Image with the marked faces: " << output << std::endl;
	}

	return 0;
}
