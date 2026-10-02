#pragma once

#include <Cam-Core.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct EmailConfig
{
	/// <summary>
	/// Turns the emails on. The other settings must be filled in then.
	/// </summary>
	bool Enabled = false;

	/// <summary>
	/// The mail server (SMTP) and its port. 587 is the usual port for "starttls", 465 for "ssl", 25 for "none".
	/// </summary>
	std::string Server;
	uint16 Port = 587;

	/// <summary>
	/// How the connection to the mail server is secured: "starttls" (the connection starts in plain text and is upgraded, port 587), "ssl" (encrypted
	/// from the first byte, port 465) or "none" (not encrypted, only for a mail server in your own network).
	/// </summary>
	std::string Security = "starttls";

	/// <summary>
	/// The login at the mail server. Empty if the server needs no login.
	/// </summary>
	std::string User;
	std::string Password;

	/// <summary>
	/// The sender of the emails, and the recipients.
	/// </summary>
	std::string From;
	std::vector<std::string> To;

	/// <summary>
	/// Written in front of the subject of every email.
	/// </summary>
	std::string SubjectPrefix = "[CamVision]";

	/// <summary>
	/// Attaches the picture with the marked face to the email.
	/// </summary>
	bool AttachSnapshot = true;

	/// <summary>
	/// true: events are collected and sent together in one email (see CollectSeconds). false: every event is sent as an email of its own (still at
	/// most one email per MinIntervalSeconds, events in between wait for their turn, they are not dropped).
	/// </summary>
	bool BatchEvents = true;

	/// <summary>
	/// The first event starts a collection window of this many seconds, everything that happens in this time is sent in ONE email (an unknown person
	/// seen by two cameras, or several cameras, which go offline together, are one email, not several). 0 sends every event on its own.
	/// </summary>
	uint32 CollectSeconds = 10;

	/// <summary>
	/// At most one email per this time. Events in between are not lost, they are collected and sent together with the next email.
	/// </summary>
	uint32 MinIntervalSeconds = 60;

	/// <summary>
	/// The maximum number of pictures in one email.
	/// </summary>
	uint32 MaxAttachments = 4;

	/// <summary>
	/// Sends an email, when a camera stops sending (it timed out), and when it is connected again. Works without the face analysis.
	/// </summary>
	bool ReportCameraOffline = true;

	/// <summary>
	/// Checks the certificate of the mail server. Only turn this off for a server in your own network with a self signed certificate.
	/// </summary>
	bool VerifyCertificate = true;

	/// <summary>
	/// The program, which talks to the mail server: curl (included in Windows 10 and 11, on Linux: sudo apt install curl).
	/// </summary>
	std::string CurlPath = "curl";

	/// <summary>
	/// How long sending an email may take.
	/// </summary>
	uint32 TimeoutSeconds = 60;
};

struct EmailAttachment
{
	std::string Name = "snapshot.jpg";

	/// <summary>
	/// A JPEG picture.
	/// </summary>
	std::vector<unsigned char> Data;
};

struct EmailMessage
{
	std::string Subject;
	std::string Body;

	/// <summary>
	/// Optional pictures, which are attached to the email.
	/// </summary>
	std::vector<EmailAttachment> Attachments;
};

/// <summary>
/// Sends emails. The emails are sent on a thread of its own, so the program does not wait for the mail server. The connection to the mail server
/// is made by curl, which takes care of the encryption and the login. Everything the mail server needs to know is written into a temporary file
/// (not onto the command line, where other users could see the password), which is deleted right after the email was sent.
/// </summary>
class Mailer
{
public:

	Mailer(const EmailConfig &config);
	~Mailer();

	/// <summary>
	/// Checks the settings and whether curl can be started. Logs what is wrong.
	/// </summary>
	/// <returns>Returns true, if emails can be sent.</returns>
	bool Validate();

	/// <summary>
	/// Puts an email into the queue, it is sent in the background. When to send is decided by the caller (see Notifier).
	/// </summary>
	void Enqueue(const EmailMessage &message);

	/// <summary>
	/// Sends an email right now and waits until it is sent.
	/// </summary>
	/// <param name="error">Receives the reason, if it failed.</param>
	bool SendNow(const EmailMessage &message, std::string *error);

private:

	void WorkerLoop();

private:

	EmailConfig m_Config;

	std::mutex m_Mutex;
	std::condition_variable m_Condition;
	std::deque<EmailMessage> m_Queue;
	std::thread m_Worker;
	bool m_Stop = false;
};
