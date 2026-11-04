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

#ifdef CAM_HAS_FACE_API
// The YuNet detector halves the picture several times and adds the levels together again, which only fits, if the width and the height are
// multiples of 32 (OpenCV 4.5.x fails with "Eltwise ... inputs[vecIdx][j] == inputs[i][j]" otherwise, for example for 640x360). The picture is
// padded at the right and bottom, so the coordinates of the faces stay the same.
static cv::Mat PadToMultipleOf32(const cv::Mat &image)
{
	int width = (image.cols + 31) / 32 * 32;
	int height = (image.rows + 31) / 32 * 32;
	if (width == image.cols && height == image.rows)
	{
		return image;
	}

	cv::Mat padded;
	cv::copyMakeBorder(image, padded, 0, height - image.rows, 0, width - image.cols, cv::BORDER_CONSTANT, cv::Scalar());
	return padded;
}
#endif

// Checks, if a detection (one row of the detector: box, 5 landmarks, score) looks like a face. The values have to be in the same coordinates (any scale).
// Right eye and left eye are those of the person, so the right eye is on the left side of the picture. Returns false and the reason otherwise.
static bool LooksLikeFace(const cv::Mat &face, const FaceConfig &config, std::string &reason)
{
	float x = face.at<float>(0, 0), y = face.at<float>(0, 1), w = face.at<float>(0, 2), h = face.at<float>(0, 3);
	char text[96];
	if (h <= 0.0f || w <= 0.0f)
	{
		reason = "empty box";
		return false;
	}

	float aspect = w / h;
	if (aspect < config.MinAspectRatio || aspect > config.MaxAspectRatio)
	{
		snprintf(text, sizeof(text), "box ratio %.2f (allowed %.2f - %.2f)", aspect, config.MinAspectRatio, config.MaxAspectRatio);
		reason = text;
		return false;
	}

	if (!config.CheckLandmarks)
	{
		return true;
	}

	float lx[5], ly[5];
	for (int i = 0; i < 5; ++i)
	{
		lx[i] = face.at<float>(0, 4 + i * 2);
		ly[i] = face.at<float>(0, 5 + i * 2);
	}

	// right eye, left eye, nose, right and left corner of the mouth
	const float tolerance_x = w * 0.3f, tolerance_y = h * 0.3f;
	for (int i = 0; i < 5; ++i)
	{
		if (lx[i] < x - tolerance_x || lx[i] > x + w + tolerance_x || ly[i] < y - tolerance_y || ly[i] > y + h + tolerance_y)
		{
			reason = "landmark outside of the box";
			return false;
		}
	}

	float eye_distance = lx[1] - lx[0];
	if (eye_distance < w * 0.1f || eye_distance > w * 0.95f)
	{
		reason = "eyes at an impossible distance";
		return false;
	}

	if (std::fabs(ly[0] - ly[1]) > eye_distance * 1.0f)
	{
		reason = "eyes not on one level";
		return false;
	}

	float eyes_y = (ly[0] + ly[1]) * 0.5f;
	float mouth_y = (ly[3] + ly[4]) * 0.5f;
	if (!(eyes_y < ly[2] && ly[2] < mouth_y))
	{
		reason = "eyes, nose and mouth are not in this order from top to bottom";
		return false;
	}

	// Turned heads (profile) have the nose far to one side, so this is generous.
	if (lx[2] < lx[0] - eye_distance * 0.9f || lx[2] > lx[1] + eye_distance * 0.9f)
	{
		reason = "nose is not between the eyes";
		return false;
	}

	if (lx[4] <= lx[3])
	{
		reason = "mouth corners are the wrong way round";
		return false;
	}

	return true;
}

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

			small = PadToMultipleOf32(small);
			m_Impl->Detector->setInputSize(small.size());
			cv::Mat faces;
			m_Impl->Detector->detect(small, faces);
			if (faces.empty() || faces.rows < 1)
			{
				CAM_LOG_WARN("No face found in the photo {}, it is not used.", photo.second.string());
				++without_face;
				continue;
			}

			// The photo should show one person. If there are more faces, the biggest one, which looks like a face, is used.
			int best = -1;
			float best_area = 0.0f;
			std::string reason = "no face looks like a face";
			for (int i = 0; i < faces.rows; ++i)
			{
				float area = faces.at<float>(i, 2) * faces.at<float>(i, 3);
				if (area <= best_area)
				{
					continue;
				}

				cv::Mat candidate = faces.row(i);
				if (!LooksLikeFace(candidate, m_Config, reason))
				{
					continue;
				}

				if (candidate.at<float>(0, 14) < m_Config.EnrollMinScore)
				{
					reason = "detector score " + std::to_string(candidate.at<float>(0, 14)) + " is below the minimum for photos (" + std::to_string(m_Config.EnrollMinScore) + ")";
					continue;
				}

				if (m_Config.EnrollMinFaceSize > 0 && std::min(candidate.at<float>(0, 2), candidate.at<float>(0, 3)) * scale < m_Config.EnrollMinFaceSize)
				{
					reason = "the face is smaller than " + std::to_string(m_Config.EnrollMinFaceSize) + " pixels";
					continue;
				}

				best_area = area;
				best = i;
			}

			if (best < 0)
			{
				CAM_LOG_WARN("The photo {0} is not used: {1}. Use a sharp photo, in which the face is large and looks at the camera.", photo.second.string(), reason);
				++without_face;
				continue;
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
		if (person.second.size() < m_Config.RecommendedPhotos)
		{
			CAM_LOG_WARN("{0} has only {1} usable photo(s). {2} or more photos (different light, angle, with and without glasses) recognize much more reliably.", person.first, person.second.size(), m_Config.RecommendedPhotos);
		}
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

std::vector<FaceResult> FaceAnalyzer::Analyze(const cv::Mat &frame, std::vector<std::string> *rejections)
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

	small = PadToMultipleOf32(small);
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
		// Detections, which cannot be faces, are dropped before the (slow) recognition: too small, wrong shape, landmarks in the wrong places.
		cv::Mat face = faces.row(i).clone();
		std::string reason;
		bool plausible = true;
		if (std::min(face.at<float>(0, 2), face.at<float>(0, 3)) < (float)m_Config.MinDetectFaceSize)
		{
			reason = "smaller than " + std::to_string(m_Config.MinDetectFaceSize) + " pixels in the scaled down frame";
			plausible = false;
		}
		else if (std::min(face.at<float>(0, 2), face.at<float>(0, 3)) * scale < (float)m_Config.MinFaceSize)
		{
			reason = "smaller than " + std::to_string(m_Config.MinFaceSize) + " pixels in the original frame";
			plausible = false;
		}
		else
		{
			plausible = LooksLikeFace(face, m_Config, reason);
		}

		if (!plausible)
		{
			if (rejections)
			{
				char text[256];
				snprintf(text, sizeof(text), "position %d,%d size %dx%d score %.2f: %s", (int)(face.at<float>(0, 0) * scale), (int)(face.at<float>(0, 1) * scale),
					(int)(face.at<float>(0, 2) * scale), (int)(face.at<float>(0, 3) * scale), face.at<float>(0, 14), reason.c_str());
				rejections->push_back(text);
			}

			continue;
		}

		// Box and landmarks (the first 14 values) in the coordinates of the original frame.
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

		// Night-time false detections are usually a box over an almost completely dark and/or flat area (an empty corner, a shadow, sensor noise),
		// which the landmark check does not reliably catch, because the "landmarks" are just as meaningless as the box. A real face, even lit only by
		// the IR illuminator, is both brighter and has more local contrast (eyes, nose, mouth) than that.
		if (m_Config.MinFaceBrightness > 0.0f || m_Config.MinFaceContrast > 0.0f)
		{
			cv::Mat gray;
			cv::cvtColor(frame(result.Box), gray, cv::COLOR_BGR2GRAY);
			cv::Scalar mean, stddev;
			cv::meanStdDev(gray, mean, stddev);
			if (mean[0] < m_Config.MinFaceBrightness || stddev[0] < m_Config.MinFaceContrast)
			{
				if (rejections)
				{
					char text[256];
					snprintf(text, sizeof(text), "position %d,%d size %dx%d score %.2f: too dark or flat to be a face (brightness %.1f, contrast %.1f)",
						result.Box.x, result.Box.y, result.Box.width, result.Box.height, result.Score, mean[0], stddev[0]);
					rejections->push_back(text);
				}

				continue;
			}
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
				else if (best >= m_Config.UncertainSimilarity)
				{
					// Not the same person as a known one, but not far away either: only a clear and big face is called an unknown person.
					bool reliable = result.Box.width >= m_Config.ReliableFaceSize && result.Box.height >= m_Config.ReliableFaceSize && result.Score >= m_Config.ReliableScore;
					result.Uncertain = !reliable;
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

static float IntersectionOverUnion(const cv::Rect &a, const cv::Rect &b)
{
	int intersection = (a & b).area();
	int total = a.area() + b.area() - intersection;
	return total > 0 ? (float)intersection / (float)total : 0.0f;
}

FaceTracker::FaceTracker(const FaceConfig &config)
	: m_Config(config)
{
}

std::vector<FaceResult> FaceTracker::Update(const std::vector<FaceResult> &faces, int64 nowMS, const std::string &camera)
{
	// Tracks, which were not seen for a while, are gone (also resets the "static" timer of a face, which was away).
	const int64 forget_ms = 10000;
	const size_t window = std::max<size_t>(m_Config.ConfirmWindow, m_Config.ConfirmFrames);
	const size_t needed = std::max<uint32>(m_Config.ConfirmFrames, 1);

	std::vector<bool> seen(m_Tracks.size(), false);
	std::vector<FaceResult> result;
	std::vector<Track> created;

	for (const FaceResult &input : faces)
	{
		FaceResult face = input;

		// A person walking by moves a lot between two analyses (only a few per second), so the boxes often do not overlap anymore.
		// A face also belongs to a track, if its center is less than about one face width away from the last one.
		int best = -1;
		float best_distance = 1.0f;
		for (size_t t = 0; t < m_Tracks.size(); ++t)
		{
			if (seen[t])
			{
				continue;
			}

			const cv::Rect &last = m_Tracks[t].Box;
			float dx = (face.Box.x + face.Box.width * 0.5f) - (last.x + last.width * 0.5f);
			float dy = (face.Box.y + face.Box.height * 0.5f) - (last.y + last.height * 0.5f);
			float size = (float)std::max(std::max(face.Box.width, face.Box.height), std::max(last.width, last.height));
			float distance = std::sqrt(dx * dx + dy * dy) / std::max(size, 1.0f);
			if (IntersectionOverUnion(face.Box, last) > 0.2f)
			{
				distance = std::min(distance, 0.5f);
			}

			if (distance < best_distance)
			{
				best_distance = distance;
				best = (int)t;
			}
		}

		Track fresh;
		Track &track = best >= 0 ? m_Tracks[best] : fresh;
		if (best >= 0)
		{
			seen[best] = true;
		}
		else
		{
			track.Anchor = face.Box;
			track.AnchorSinceMS = nowMS;
		}

		track.Box = face.Box;
		track.LastSeenMS = nowMS;
		track.History.push_back(true);
		while (track.History.size() > window)
		{
			track.History.pop_front();
		}

		if (IntersectionOverUnion(face.Box, track.Anchor) < 0.7f)
		{
			track.Anchor = face.Box;
			track.AnchorSinceMS = nowMS;
			track.StaticLogged = false;
		}

		// The person was recognized a moment ago and is the same face: a turned head or a blurred frame does not make a stranger of them.
		if (face.Recognized)
		{
			track.Name = face.Name;
			track.Similarity = face.Similarity;
			track.RecognizedMS = nowMS;
		}
		else if (!track.Name.empty() && nowMS - track.RecognizedMS < 5000)
		{
			face.Recognized = true;
			face.Uncertain = false;
			face.Name = track.Name;
			face.Similarity = track.Similarity;
		}

		size_t hits = (size_t)std::count(track.History.begin(), track.History.end(), true);
		bool confirmed = hits >= needed;
		bool frozen = m_Config.StaticSeconds > 0 && nowMS - track.AnchorSinceMS >= (int64)m_Config.StaticSeconds * 1000;
		if (confirmed && frozen)
		{
			if (!track.StaticLogged)
			{
				CAM_LOG_INFO("{0}: ignoring a face at {1},{2} ({3}x{4}), it did not move for {5} seconds, so it is part of the scene.", camera, face.Box.x, face.Box.y, face.Box.width, face.Box.height, m_Config.StaticSeconds);
				track.StaticLogged = true;
			}
		}
		else if (confirmed)
		{
			result.push_back(face);
		}

		if (best < 0)
		{
			created.push_back(track);
		}
	}

	// Tracks without a face in this analysis: a miss.
	for (size_t t = 0; t < m_Tracks.size(); ++t)
	{
		if (!seen[t])
		{
			m_Tracks[t].History.push_back(false);
			while (m_Tracks[t].History.size() > window)
			{
				m_Tracks[t].History.pop_front();
			}
		}
	}

	m_Tracks.erase(std::remove_if(m_Tracks.begin(), m_Tracks.end(), [&](const Track &track) { return nowMS - track.LastSeenMS > forget_ms; }), m_Tracks.end());
	m_Tracks.insert(m_Tracks.end(), created.begin(), created.end());
	return result;
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
		else if (face.Uncertain)
		{
			color = cv::Scalar(0, 165, 255);	// orange
			label = "uncertain";
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

	std::vector<std::string> rejections;
	std::vector<FaceResult> faces = analyzer.Analyze(image, &rejections);
	std::cout << "Faces found in " << imagePath << ": " << faces.size() << std::endl;
	for (const std::string &rejection : rejections)
	{
		std::cout << "  Dropped (not a face): " << rejection << std::endl;
	}

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
			else if (face.Uncertain)
			{
				std::cout << " -> uncertain (best similarity " << face.Similarity << ", needed " << config.MatchThreshold << ", but the face is small or the detector is not sure)";
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
