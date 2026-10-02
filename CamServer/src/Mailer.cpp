#include "Mailer.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "Core/Log.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	std::string RandomHex(int bytes)
	{
		static std::mt19937_64 generator{ std::random_device{}() };
		static const char *digits = "0123456789abcdef";
		std::string result;
		for (int i = 0; i < bytes; ++i)
		{
			unsigned int value = (unsigned int)(generator() & 0xFF);
			result += digits[value >> 4];
			result += digits[value & 15];
		}

		return result;
	}

	// Base64 in lines of 76 characters, as email requires it.
	std::string Base64(const unsigned char *data, size_t size)
	{
		static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string encoded;
		size_t line_length = 0;
		for (size_t i = 0; i < size; i += 3)
		{
			unsigned int block = (unsigned int)data[i] << 16;
			if (i + 1 < size) block |= (unsigned int)data[i + 1] << 8;
			if (i + 2 < size) block |= (unsigned int)data[i + 2];

			encoded += table[(block >> 18) & 63];
			encoded += table[(block >> 12) & 63];
			encoded += (i + 1 < size) ? table[(block >> 6) & 63] : '=';
			encoded += (i + 2 < size) ? table[block & 63] : '=';

			line_length += 4;
			if (line_length >= 76)
			{
				encoded += "\r\n";
				line_length = 0;
			}
		}

		if (line_length != 0)
		{
			encoded += "\r\n";
		}

		return encoded;
	}

	// Headers may only contain ASCII. Everything else is encoded as "encoded word" (RFC 2047).
	std::string EncodeHeader(const std::string &text)
	{
		bool ascii = std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c < 127; });
		if (ascii)
		{
			return text;
		}

		std::string encoded = Base64((const unsigned char *)text.data(), text.size());
		encoded.erase(std::remove(encoded.begin(), encoded.end(), '\r'), encoded.end());
		encoded.erase(std::remove(encoded.begin(), encoded.end(), '\n'), encoded.end());
		return "=?UTF-8?B?" + encoded + "?=";
	}

	std::string DateHeader()
	{
		std::time_t now = std::time(nullptr);
		std::tm local = {};
#ifdef _WIN32
		localtime_s(&local, &now);
#else
		localtime_r(&now, &local);
#endif

		char buffer[64];
		strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S %z", &local);
		return buffer;
	}

	// The email, as it is sent: headers, text and the optional picture, all with CRLF line ends.
	std::string BuildMessage(const EmailConfig &config, const EmailMessage &message)
	{
		std::string to;
		for (const std::string &recipient : config.To)
		{
			to += (to.empty() ? "" : ", ") + recipient;
		}

		std::string subject = config.SubjectPrefix.empty() ? message.Subject : config.SubjectPrefix + " " + message.Subject;
		std::string boundary = "camvision-" + RandomHex(12);

		std::string text = message.Body;
		std::string normalized;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (text[i] == '\r')
			{
				continue;
			}

			if (text[i] == '\n')
			{
				normalized += "\r\n";
			}
			else
			{
				normalized += text[i];
			}
		}

		std::string email;
		email += "Date: " + DateHeader() + "\r\n";
		email += "From: " + config.From + "\r\n";
		email += "To: " + to + "\r\n";
		email += "Subject: " + EncodeHeader(subject) + "\r\n";
		email += "Message-ID: <" + RandomHex(16) + "@camvision>\r\n";
		email += "MIME-Version: 1.0\r\n";

		bool attach = !message.Attachments.empty();
		if (attach)
		{
			email += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";
			email += "--" + boundary + "\r\n";
		}

		email += "Content-Type: text/plain; charset=UTF-8\r\n";
		email += "Content-Transfer-Encoding: base64\r\n\r\n";
		email += Base64((const unsigned char *)normalized.data(), normalized.size());

		if (attach)
		{
			for (const EmailAttachment &attachment : message.Attachments)
			{
				email += "--" + boundary + "\r\n";
				email += "Content-Type: image/jpeg; name=\"" + attachment.Name + "\"\r\n";
				email += "Content-Transfer-Encoding: base64\r\n";
				email += "Content-Disposition: attachment; filename=\"" + attachment.Name + "\"\r\n\r\n";
				email += Base64(attachment.Data.data(), attachment.Data.size());
			}

			email += "--" + boundary + "--\r\n";
		}

		return email;
	}

	// A value in the configuration file of curl: in quotes, with backslashes and quotes escaped.
	std::string CurlQuote(const std::string &value)
	{
		std::string quoted = "\"";
		for (char c : value)
		{
			if (c == '\\' || c == '"')
			{
				quoted += '\\';
				quoted += c;
			}
			else if (c == '\n')
			{
				quoted += "\\n";
			}
			else if (c != '\r')
			{
				quoted += c;
			}
		}

		return quoted + "\"";
	}

	// Writes a file, which only the current user can read (it may contain the password).
	bool WritePrivateFile(const std::filesystem::path &path, const std::string &content)
	{
#ifdef _WIN32
		// The temporary folder of Windows belongs to the current user.
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			return false;
		}

		file.write(content.data(), (std::streamsize)content.size());
		return (bool)file;
#else
		int fd = open(path.string().c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0)
		{
			return false;
		}

		size_t written = 0;
		while (written < content.size())
		{
			ssize_t n = write(fd, content.data() + written, content.size() - written);
			if (n <= 0)
			{
				close(fd);
				return false;
			}

			written += (size_t)n;
		}

		close(fd);
		return true;
#endif
	}

	std::string ReadFileText(const std::filesystem::path &path, size_t maxBytes)
	{
		std::ifstream file(path, std::ios::binary);
		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (text.size() > maxBytes)
		{
			text.resize(maxBytes);
		}

		while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
		{
			text.pop_back();
		}

		return text;
	}

	// Runs a program and waits for it. Everything it prints is written into outputFile. Returns its exit code, or -1 if it could not be started.
	int RunProcess(const std::string &program, const std::vector<std::string> &arguments, const std::filesystem::path &outputFile, uint32 timeoutSeconds)
	{
#ifdef _WIN32
		std::string command = "\"" + program + "\"";
		for (const std::string &argument : arguments)
		{
			command += " \"" + argument + "\"";
		}

		SECURITY_ATTRIBUTES inheritable = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		HANDLE output = CreateFileA(outputFile.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE)
		{
			if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
			if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
			return -1;
		}

		STARTUPINFOA startup = {};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = input;
		startup.hStdOutput = output;
		startup.hStdError = output;

		PROCESS_INFORMATION process = {};
		std::vector<char> mutable_command(command.begin(), command.end());
		mutable_command.push_back('\0');
		BOOL started = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
		CloseHandle(output);
		CloseHandle(input);
		if (!started)
		{
			return -1;
		}

		int result = -1;
		if (WaitForSingleObject(process.hProcess, (timeoutSeconds + 10) * 1000) == WAIT_TIMEOUT)
		{
			TerminateProcess(process.hProcess, 1);
		}
		else
		{
			DWORD exit_code = 0;
			if (GetExitCodeProcess(process.hProcess, &exit_code))
			{
				result = (int)exit_code;
			}
		}

		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
		return result;
#else
		std::vector<std::string> storage;
		storage.push_back(program);
		storage.insert(storage.end(), arguments.begin(), arguments.end());
		std::vector<char *> argv;
		for (std::string &s : storage)
		{
			argv.push_back(&s[0]);
		}
		argv.push_back(nullptr);

		pid_t pid = fork();
		if (pid < 0)
		{
			return -1;
		}

		if (pid == 0)
		{
			int out = open(outputFile.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
			int in = open("/dev/null", O_RDONLY);
			if (out < 0 || in < 0)
			{
				_exit(126);
			}

			dup2(in, 0);
			dup2(out, 1);
			dup2(out, 2);
			execvp(argv[0], argv.data());
			_exit(127);	// the program was not found
		}

		int status = 0;
		if (waitpid(pid, &status, 0) < 0)
		{
			return -1;
		}

		if (WIFEXITED(status))
		{
			int code = WEXITSTATUS(status);
			return code == 127 ? -1 : code;
		}

		return -1;
#endif
	}

	// The most important reasons, why curl fails to send an email, in plain words.
	std::string ExplainCurlExitCode(int code)
	{
		switch (code)
		{
			case 6:  return "the mail server was not found (check the server name)";
			case 7:  return "could not connect to the mail server (check server, port and firewall)";
			case 28: return "the mail server did not answer in time";
			case 35: return "the TLS handshake failed (check the port and the security setting: starttls for 587, ssl for 465)";
			case 55:
			case 56: return "the connection to the mail server was interrupted";
			case 60:
			case 51: return "the certificate of the mail server is not trusted (a self signed certificate needs email_verify_certificate = false)";
			case 64: return "the mail server needs a secure connection (set email_security to starttls or ssl)";
			case 67: return "the mail server refused the login (check email_user and email_password; some providers need an app password)";
			case 9:  return "the mail server refused the sender or a recipient (check email_from and email_to)";
			default: return "";
		}
	}
}

Mailer::Mailer(const EmailConfig &config)
	: m_Config(config)
{
	m_Worker = std::thread(&Mailer::WorkerLoop, this);
}

Mailer::~Mailer()
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

bool Mailer::Validate()
{
	bool valid = true;
	if (m_Config.Server.empty())
	{
		CAM_LOG_ERROR("Emails: email_smtp_server is not set.");
		valid = false;
	}

	if (m_Config.From.empty())
	{
		CAM_LOG_ERROR("Emails: email_from is not set.");
		valid = false;
	}

	if (m_Config.To.empty())
	{
		CAM_LOG_ERROR("Emails: email_to is not set.");
		valid = false;
	}

	if (m_Config.Security != "starttls" && m_Config.Security != "ssl" && m_Config.Security != "none")
	{
		CAM_LOG_ERROR("Emails: email_security must be starttls, ssl or none, not '{}'.", m_Config.Security);
		valid = false;
	}

	if (m_Config.Security == "none" && !m_Config.User.empty())
	{
		CAM_LOG_WARN("Emails: the login is sent without encryption (email_security = none).");
	}

	if (!valid)
	{
		return false;
	}

	// Is curl there?
	std::error_code error;
	std::filesystem::path output = std::filesystem::temp_directory_path(error) / ("camvision_" + RandomHex(6) + ".txt");
	int code = RunProcess(m_Config.CurlPath, { "--version" }, output, 10);
	std::filesystem::remove(output, error);
	if (code != 0)
	{
		CAM_LOG_ERROR("Emails: curl ('{}') could not be started. It is included in Windows 10 and 11, on Linux install it with: sudo apt install curl", m_Config.CurlPath);
		return false;
	}

	return true;
}

void Mailer::Enqueue(const EmailMessage &message)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_Queue.push_back(message);
	m_Condition.notify_one();
}

void Mailer::WorkerLoop()
{
	for (;;)
	{
		EmailMessage message;
		{
			std::unique_lock<std::mutex> lock(m_Mutex);
			m_Condition.wait(lock, [this] { return m_Stop || !m_Queue.empty(); });
			if (m_Queue.empty())
			{
				return;
			}

			message = std::move(m_Queue.front());
			m_Queue.pop_front();
		}

		std::string error;
		if (SendNow(message, &error))
		{
			CAM_LOG_INFO("Email '{}' sent.", message.Subject);
		}
		else
		{
			CAM_LOG_ERROR("Could not send the email '{0}': {1}", message.Subject, error);
		}
	}
}

bool Mailer::SendNow(const EmailMessage &message, std::string *error)
{
	std::error_code file_error;
	std::filesystem::path temp = std::filesystem::temp_directory_path(file_error);
	if (file_error)
	{
		*error = "there is no temporary folder: " + file_error.message();
		return false;
	}

	std::string id = RandomHex(8);
	std::filesystem::path email_file = temp / ("camvision_mail_" + id + ".eml");
	std::filesystem::path config_file = temp / ("camvision_mail_" + id + ".cfg");
	std::filesystem::path output_file = temp / ("camvision_mail_" + id + ".txt");

	// Everything is removed in any case, the configuration file contains the password.
	auto cleanup = [&]()
	{
		std::error_code ignore;
		std::filesystem::remove(email_file, ignore);
		std::filesystem::remove(config_file, ignore);
		std::filesystem::remove(output_file, ignore);
	};

	std::string email = BuildMessage(m_Config, message);
	if (!WritePrivateFile(email_file, email))
	{
		*error = "could not write the temporary file " + email_file.string();
		cleanup();
		return false;
	}

	std::string scheme = m_Config.Security == "ssl" ? "smtps" : "smtp";
	std::ostringstream curl_config;
	curl_config << "silent\n";
	curl_config << "show-error\n";
	curl_config << "connect-timeout = 20\n";
	curl_config << "max-time = " << m_Config.TimeoutSeconds << "\n";
	curl_config << "url = " << CurlQuote(scheme + "://" + m_Config.Server + ":" + std::to_string(m_Config.Port)) << "\n";
	if (m_Config.Security == "starttls")
	{
		curl_config << "ssl-reqd\n";
	}

	if (!m_Config.VerifyCertificate)
	{
		curl_config << "insecure\n";
	}

	curl_config << "mail-from = " << CurlQuote("<" + m_Config.From + ">") << "\n";
	for (const std::string &recipient : m_Config.To)
	{
		curl_config << "mail-rcpt = " << CurlQuote("<" + recipient + ">") << "\n";
	}

	if (!m_Config.User.empty())
	{
		curl_config << "user = " << CurlQuote(m_Config.User + ":" + m_Config.Password) << "\n";
	}

	curl_config << "upload-file = " << CurlQuote(email_file.string()) << "\n";

	if (!WritePrivateFile(config_file, curl_config.str()))
	{
		*error = "could not write the temporary file " + config_file.string();
		cleanup();
		return false;
	}

	int code = RunProcess(m_Config.CurlPath, { "--config", config_file.string() }, output_file, m_Config.TimeoutSeconds);
	std::string output = ReadFileText(output_file, 600);
	cleanup();

	if (code == 0)
	{
		return true;
	}

	if (code < 0)
	{
		*error = "curl ('" + m_Config.CurlPath + "') could not be started or did not finish";
		return false;
	}

	std::string explanation = ExplainCurlExitCode(code);
	*error = "curl failed with exit code " + std::to_string(code) + (explanation.empty() ? "" : ": " + explanation) + (output.empty() ? "" : " (" + output + ")");
	return false;
}
