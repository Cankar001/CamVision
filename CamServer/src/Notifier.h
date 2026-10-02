#pragma once

#include <Cam-Core.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Mailer.h"

/// <summary>
/// Collects the events, which are worth an email (an unknown person was seen, a camera went offline, a camera is back), and sends them as ONE email,
/// instead of one email per event:
///
///  - The first event starts a short collection window (EmailConfig::CollectSeconds). Everything that happens in this time goes into the same email.
///  - Between two emails at least EmailConfig::MinIntervalSeconds pass. Events, which happen in this time, are not lost: they are collected and sent
///    together with the next email.
///
/// So somebody walking in front of three cameras, or a power cut, which takes several cameras offline, result in one email each, not in dozens.
/// Adding an event never blocks, the emails are put together and handed to the mailer on a thread of its own.
/// </summary>
class Notifier
{
public:

	Notifier(Mailer &mailer, const EmailConfig &config);
	~Notifier();

	/// <summary>
	/// A face was seen, which matches nobody of the known people.
	/// </summary>
	/// <param name="snapshotJpeg">The picture with the marked face (JPEG), empty if there is none.</param>
	void AddUnknownPerson(const std::string &camera, float similarity, float neededSimilarity, float detectorScore, const std::vector<unsigned char> &snapshotJpeg);

	/// <summary>
	/// A camera stopped sending (it timed out).
	/// </summary>
	void AddCameraOffline(const std::string &camera, uint32 silentSeconds);

	/// <summary>
	/// A camera, which was offline, is connected again.
	/// </summary>
	void AddCameraOnline(const std::string &camera);

private:

	enum class Type
	{
		UnknownPerson,
		CameraOffline,
		CameraOnline
	};

	struct Item
	{
		Type Kind = Type::UnknownPerson;
		std::string Camera;
		std::string Date;			// 2026-10-03
		std::string Time;			// 10:15:00
		float Similarity = 0.0f;
		float Needed = 0.0f;
		float Score = 0.0f;
		uint32 SilentSeconds = 0;
		std::vector<unsigned char> Snapshot;
	};

	void Add(Item item);
	void WorkerLoop();

	/// <summary>
	/// Puts the email for a number of events together.
	/// </summary>
	EmailMessage BuildEmail(const std::vector<Item> &items) const;

private:

	Mailer &m_Mailer;
	EmailConfig m_Config;

	std::mutex m_Mutex;
	std::condition_variable m_Condition;
	std::vector<Item> m_Pending;
	std::thread m_Worker;
	bool m_Stop = false;

	int64 m_FirstPendingMS = 0;
	int64 m_LastSentMS = 0;
};
