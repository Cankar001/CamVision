#include "Notifier.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>

#include "Core/Log.h"

namespace
{
	// More events than this are not listed in the email (and not remembered), the number is still counted.
	const size_t MAX_PENDING_ITEMS = 200;

	// The list in the email does not become longer than this.
	const size_t MAX_LISTED_ITEMS = 30;

	// Only the first pictures are kept in memory, a picture is large.
	const size_t MAX_STORED_SNAPSHOTS = 12;

	void LocalTime(std::string *date, std::string *time)
	{
		std::time_t now = std::time(nullptr);
		std::tm local = {};
#ifdef _WIN32
		localtime_s(&local, &now);
#else
		localtime_r(&now, &local);
#endif

		char buffer[32];
		strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
		*date = buffer;
		strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
		*time = buffer;
	}

	// A name, which can be used in a file name.
	std::string FileName(const std::string &text)
	{
		std::string result = text;
		for (char &c : result)
		{
			if (!isalnum((unsigned char)c) && c != '-' && c != '_')
			{
				c = '_';
			}
		}

		return result.empty() ? "camera" : result;
	}

	// "Front, Garage and 2 more"
	std::string NameList(const std::vector<std::string> &names)
	{
		std::string result;
		for (size_t i = 0; i < names.size() && i < 3; ++i)
		{
			result += (i == 0 ? "" : ", ") + names[i];
		}

		if (names.size() > 3)
		{
			result += " and " + std::to_string(names.size() - 3) + " more";
		}

		return result;
	}

	void AddUnique(std::vector<std::string> &names, const std::string &name)
	{
		if (std::find(names.begin(), names.end(), name) == names.end())
		{
			names.push_back(name);
		}
	}
}

Notifier::Notifier(Mailer &mailer, const EmailConfig &config)
	: m_Mailer(mailer), m_Config(config)
{
	m_Worker = std::thread(&Notifier::WorkerLoop, this);
}

Notifier::~Notifier()
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Stop = true;
	}

	m_Condition.notify_all();
	if (m_Worker.joinable())
	{
		m_Worker.join();
	}
}

void Notifier::AddUnknownPerson(const std::string &camera, float similarity, float neededSimilarity, float detectorScore, const std::vector<unsigned char> &snapshotJpeg)
{
	Item item;
	item.Kind = Type::UnknownPerson;
	item.Camera = camera;
	item.Similarity = similarity;
	item.Needed = neededSimilarity;
	item.Score = detectorScore;
	item.Snapshot = snapshotJpeg;
	Add(std::move(item));
}

void Notifier::AddCameraOffline(const std::string &camera, uint32 silentSeconds)
{
	Item item;
	item.Kind = Type::CameraOffline;
	item.Camera = camera;
	item.SilentSeconds = silentSeconds;
	Add(std::move(item));
}

void Notifier::AddCameraOnline(const std::string &camera)
{
	Item item;
	item.Kind = Type::CameraOnline;
	item.Camera = camera;
	Add(std::move(item));
}

void Notifier::Add(Item item)
{
	LocalTime(&item.Date, &item.Time);

	{
		std::lock_guard<std::mutex> lock(m_Mutex);

		if (m_Pending.size() >= MAX_PENDING_ITEMS)
		{
			// Something is going on, which cannot be listed completely anyway. The email says how many events there were not.
			return;
		}

		// Pictures use a lot of memory, only the first ones are kept.
		size_t stored = 0;
		for (const Item &pending : m_Pending)
		{
			if (!pending.Snapshot.empty())
			{
				++stored;
			}
		}

		if (stored >= MAX_STORED_SNAPSHOTS)
		{
			item.Snapshot.clear();
		}

		if (m_Pending.empty())
		{
			m_FirstPendingMS = Core::QueryMS();
		}

		m_Pending.push_back(std::move(item));
	}

	m_Condition.notify_one();
}

void Notifier::WorkerLoop()
{
	for (;;)
	{
		std::vector<Item> items;
		{
			std::unique_lock<std::mutex> lock(m_Mutex);

			// Wait until there is something, and until it is time to send it: the collection window is over, and the pause after the last email too.
			for (;;)
			{
				if (m_Stop)
				{
					return;
				}

				if (m_Pending.empty())
				{
					m_Condition.wait(lock);
					continue;
				}

				int64 now = Core::QueryMS();

				// Batching waits for the collection window. Without batching, an event is sent as soon as the pause after the last email is over.
				int64 due = m_Config.BatchEvents ? m_FirstPendingMS + (int64)m_Config.CollectSeconds * 1000 : 0;
				if (m_LastSentMS != 0)
				{
					due = std::max<int64>(due, m_LastSentMS + (int64)m_Config.MinIntervalSeconds * 1000);
				}

				if (now >= due)
				{
					break;
				}

				// More events may arrive meanwhile, the time is calculated again when it is over.
				m_Condition.wait_for(lock, std::chrono::milliseconds(due - now));
			}

			if (m_Config.BatchEvents)
			{
				items.swap(m_Pending);
			}
			else
			{
				// One event per email, the others wait for their turn.
				items.push_back(std::move(m_Pending.front()));
				m_Pending.erase(m_Pending.begin());
			}

			m_LastSentMS = Core::QueryMS();
		}

		EmailMessage message = BuildEmail(items);
		m_Mailer.Enqueue(message);
		CAM_LOG_INFO("Email '{0}' about {1} event(s) is being sent...", message.Subject, items.size());
	}
}

EmailMessage Notifier::BuildEmail(const std::vector<Item> &items) const
{
	std::vector<std::string> unknown_cameras, offline_cameras, online_cameras;
	size_t unknown = 0, offline = 0, online = 0;
	for (const Item &item : items)
	{
		switch (item.Kind)
		{
			case Type::UnknownPerson:
				++unknown;
				AddUnique(unknown_cameras, item.Camera);
				break;

			case Type::CameraOffline:
				++offline;
				AddUnique(offline_cameras, item.Camera);
				break;

			case Type::CameraOnline:
				++online;
				AddUnique(online_cameras, item.Camera);
				break;
		}
	}

	EmailMessage message;

	// The subject says what happened, as exactly as it fits into a line.
	if (unknown > 0 && offline == 0 && online == 0)
	{
		message.Subject = unknown == 1 ? "Unknown person at " + unknown_cameras[0] : std::to_string(unknown) + " unknown people seen (" + NameList(unknown_cameras) + ")";
	}
	else if (unknown == 0 && offline + online == 1)
	{
		message.Subject = offline == 1 ? "Camera " + offline_cameras[0] + " is offline" : "Camera " + online_cameras[0] + " is back online";
	}
	else if (unknown == 0)
	{
		std::string parts;
		if (offline > 0)
		{
			parts += std::to_string(offline_cameras.size()) + (offline_cameras.size() == 1 ? " camera offline" : " cameras offline");
		}

		if (online > 0)
		{
			parts += (parts.empty() ? "" : ", ") + std::to_string(online_cameras.size()) + (online_cameras.size() == 1 ? " camera back online" : " cameras back online");
		}

		message.Subject = "Camera events: " + parts;
	}
	else
	{
		size_t camera_events = offline + online;
		message.Subject = std::to_string(unknown) + (unknown == 1 ? " unknown person seen, " : " unknown people seen, ") + std::to_string(camera_events) + (camera_events == 1 ? " camera event" : " camera events");
	}

	// The text: what happened, one line per event.
	std::string body;
	if (items.size() == 1 && items[0].Kind == Type::UnknownPerson)
	{
		const Item &item = items[0];
		char numbers[160];
		snprintf(numbers, sizeof(numbers), "%.2f (needed %.2f), detector score %.2f", item.Similarity, item.Needed, item.Score);
		body += "An unknown person was seen by the camera \"" + item.Camera + "\".\n\n";
		body += "Time: " + item.Date + " " + item.Time + "\n";
		body += "Camera: " + item.Camera + "\n";
		body += std::string("Best similarity to a known person: ") + numbers + "\n";
	}
	else
	{
		body += "CamVision report, " + items.front().Date + (items.front().Date != items.back().Date ? " to " + items.back().Date : "") + "\n";

		size_t listed = 0;
		auto section = [&](const char *title, size_t count, Type kind)
		{
			if (count == 0)
			{
				return;
			}

			body += "\n" + std::string(title) + " (" + std::to_string(count) + "):\n";
			for (const Item &item : items)
			{
				if (item.Kind != kind || listed >= MAX_LISTED_ITEMS)
				{
					continue;
				}

				++listed;
				if (kind == Type::UnknownPerson)
				{
					char numbers[96];
					snprintf(numbers, sizeof(numbers), "best similarity %.2f (needed %.2f), detector score %.2f", item.Similarity, item.Needed, item.Score);
					body += "  " + item.Time + "  " + item.Camera + ": " + numbers + "\n";
				}
				else if (kind == Type::CameraOffline)
				{
					body += "  " + item.Time + "  " + item.Camera + " went offline (nothing received for " + std::to_string(item.SilentSeconds) + " seconds)\n";
				}
				else
				{
					body += "  " + item.Time + "  " + item.Camera + " is back online\n";
				}
			}
		};

		section("Unknown people", unknown, Type::UnknownPerson);
		section("Cameras offline", offline, Type::CameraOffline);
		section("Cameras back online", online, Type::CameraOnline);

		if (listed < items.size())
		{
			body += "\n(" + std::to_string(items.size() - listed) + " more events are not listed.)\n";
		}
	}

	// The pictures: the first one of every camera first, then the following ones, up to the limit.
	std::vector<const Item *> with_picture;
	for (const Item &item : items)
	{
		if (item.Kind == Type::UnknownPerson && !item.Snapshot.empty())
		{
			with_picture.push_back(&item);
		}
	}

	std::vector<const Item *> chosen;
	std::vector<std::string> covered;
	for (const Item *item : with_picture)
	{
		if (chosen.size() < m_Config.MaxAttachments && std::find(covered.begin(), covered.end(), item->Camera) == covered.end())
		{
			covered.push_back(item->Camera);
			chosen.push_back(item);
		}
	}

	for (const Item *item : with_picture)
	{
		if (chosen.size() < m_Config.MaxAttachments && std::find(chosen.begin(), chosen.end(), item) == chosen.end())
		{
			chosen.push_back(item);
		}
	}

	std::sort(chosen.begin(), chosen.end(), [&](const Item *a, const Item *b) { return a < b; });
	for (const Item *item : chosen)
	{
		EmailAttachment attachment;
		std::string time = item->Time;
		time.erase(std::remove(time.begin(), time.end(), ':'), time.end());
		attachment.Name = "unknown_person_" + FileName(item->Camera) + "_" + time + ".jpg";
		attachment.Data = item->Snapshot;
		message.Attachments.push_back(std::move(attachment));
	}

	if (unknown > 0)
	{
		body += "\nPictures: " + (message.Attachments.empty() ? std::string("none attached") : std::to_string(message.Attachments.size()) + " attached");
		if (!message.Attachments.empty() && message.Attachments.size() < unknown)
		{
			body += " (not one for every person, see email_max_attachments)";
		}

		body += "\n";
	}

	body += "\nThis message was sent automatically by CamVision.\n";
	message.Body = body;
	return message;
}
