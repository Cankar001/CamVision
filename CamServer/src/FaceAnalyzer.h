#pragma once

#include <Cam-Core.h>

#include <memory>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

struct FaceConfig
{
	/// <summary>
	/// Turns the face detection (and recognition) on. Needs the model files, see README.
	/// </summary>
	bool Enabled = false;

	/// <summary>
	/// The face detector model (YuNet, ONNX).
	/// </summary>
	std::string DetectorModel = "models/face_detection_yunet_2023mar.onnx";

	/// <summary>
	/// The face recognizer model (SFace, ONNX). Without it, faces are only detected, not recognized.
	/// </summary>
	std::string RecognizerModel = "models/face_recognition_sface_2021dec.onnx";

	/// <summary>
	/// The folder with the photos of the known people: one sub folder per person (the name of the folder is the name of the person),
	/// which contains one or more photos of that person. Photos directly in the folder work too, the file name is the name of the person.
	/// </summary>
	std::string KnownFacesPath = "known_faces";

	/// <summary>
	/// Minimum confidence (0 - 1) of the detector for a face.
	/// </summary>
	float ScoreThreshold = 0.7f;

	/// <summary>
	/// Minimum cosine similarity (-1 - 1) between a face and a known face, to count as the same person. 0.363 is the value recommended by the authors of the model.
	/// Higher is stricter (fewer wrong matches, but more known people are not recognized).
	/// </summary>
	float MatchThreshold = 0.363f;

	/// <summary>
	/// Frames are scaled down to this width for the detection, which makes it much faster (faces are still found in good quality).
	/// The recognition always uses the original resolution. 0 does not scale.
	/// </summary>
	int32 DetectWidth = 640;

	/// <summary>
	/// How often the frames of every camera are analyzed per second.
	/// </summary>
	uint32 FPS = 3;

	/// <summary>
	/// Stores a photo with the marked faces, when a person (or a face) is seen for the first time or again after the cooldown.
	/// </summary>
	bool Snapshots = false;

	/// <summary>
	/// The folder for the snapshots (one sub folder for every camera).
	/// </summary>
	std::string SnapshotPath = "faces";

	/// <summary>
	/// A person, who stays in front of a camera, is reported once per this time.
	/// </summary>
	uint32 EventCooldownSeconds = 30;
};

struct FaceResult
{
	/// <summary>
	/// The face in the coordinates of the analyzed frame.
	/// </summary>
	cv::Rect Box;

	/// <summary>
	/// Confidence of the detector (0 - 1).
	/// </summary>
	float Score = 0.0f;

	/// <summary>
	/// True, if the face was matched with a known person.
	/// </summary>
	bool Recognized = false;

	/// <summary>
	/// The name of the best matching known person (only set, if Recognized).
	/// </summary>
	std::string Name;

	/// <summary>
	/// The cosine similarity to the best matching known person (0, if nobody is known).
	/// </summary>
	float Similarity = 0.0f;
};

/// <summary>
/// Detects faces in frames (YuNet) and recognizes them by comparing them with the photos of known people (SFace).
/// The models are part of OpenCV (version 4.5.4 or newer), no other library is needed.
/// Not thread safe: use one analyzer from one thread at a time.
/// </summary>
class FaceAnalyzer
{
public:

	FaceAnalyzer(const FaceConfig &config);
	~FaceAnalyzer();

	/// <summary>
	/// Loads the models and the known faces. Logs the reason, if that is not possible (for example missing model files).
	/// </summary>
	/// <returns>Returns true, if at least the detection works.</returns>
	bool Initialize();

	/// <summary>
	/// True, if recognition is possible: the recognizer model is loaded and at least one known face is enrolled.
	/// </summary>
	bool CanRecognize() const;

	/// <summary>
	/// The number of known people.
	/// </summary>
	uint32 GetKnownPeopleCount() const;

	/// <summary>
	/// Finds all faces in the frame, and recognizes them, if recognition is possible.
	/// </summary>
	/// <param name="frame">A color frame (BGR).</param>
	std::vector<FaceResult> Analyze(const cv::Mat &frame);

	/// <summary>
	/// Loads the known faces again, if photos were added, changed or removed in the known faces folder.
	/// </summary>
	void ReloadKnownFacesIfChanged();

private:

	void LoadKnownFaces();

	struct Impl;
	std::unique_ptr<Impl> m_Impl;
	FaceConfig m_Config;
};

/// <summary>
/// Draws the boxes of the faces and their names into the frame. Green: known person, red: unknown person, yellow: face (no recognition possible).
/// </summary>
void DrawFaces(cv::Mat &frame, const std::vector<FaceResult> &faces, bool recognitionActive);

/// <summary>
/// Analyzes a single image and prints the result to the console (to check, that the models and the known faces work). An image with the marked faces is stored
/// next to the original. Returns the exit code of the program.
/// </summary>
int RunFaceTest(const FaceConfig &config, const std::string &imagePath);
