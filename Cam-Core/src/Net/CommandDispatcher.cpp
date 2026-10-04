#include "CommandDispatcher.h"

#include "Core/CryptoPrimitives.h"
#include "Core/Log.h"
#include "Core/Timer.h"

#include <algorithm>
#include <cstring>

namespace Core
{
	namespace
	{
		constexpr int MAX_FAILED_ATTEMPTS = 3;
	}

	CommandDispatcher::CommandDispatcher(const std::string &token)
		: m_Token(token)
	{
	}

	void CommandDispatcher::Register(const std::string &name, const std::string &description, Handler handler)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Commands[name] = { description, std::move(handler) };
	}

	WebSocketServer::MessageHandler CommandDispatcher::AsMessageHandler()
	{
		return [this](WebSocketSession &session, const std::string &message, std::string *answer, bool *closeConnection)
		{
			*answer = Process(&session, message, closeConnection);
		};
	}

	std::string CommandDispatcher::MakeEvent(const std::string &name, const Json &data)
	{
		Json event = Json::Object();
		event["event"] = name;
		event["data"] = data;
		return event.Dump();
	}

	std::string CommandDispatcher::Answer(const Json &id, bool ok, const Json &result, const std::string &error) const
	{
		Json answer = Json::Object();
		if (!id.IsNull())
		{
			answer["id"] = id;
		}

		answer["ok"] = ok;
		if (ok)
		{
			answer["result"] = result.IsNull() ? Json::Object() : result;
		}
		else
		{
			answer["error"] = error;
		}

		return answer.Dump();
	}

	std::string CommandDispatcher::Process(WebSocketSession *session, const std::string &message, bool *closeConnection)
	{
		*closeConnection = false;

		Json request;
		std::string parse_error;
		if (!Json::Parse(message, &request, &parse_error) || !request.IsObject())
		{
			return Answer(Json(), false, Json(), "the message is not a JSON object: " + (parse_error.empty() ? std::string("it is not an object") : parse_error));
		}

		const Json &id = request.Get("id");
		std::string name = request.Get("cmd").AsString();
		if (name.empty())
		{
			return Answer(id, false, Json(), "the message has no \"cmd\"");
		}

		Json no_arguments = Json::Object();
		const Json *given = request.Find("args");
		const Json &args = given && given->IsObject() ? *given : no_arguments;

		// ---- the login
		if (name == "auth")
		{
			if (m_Token.empty())
			{
				// A server without a secret lets nobody in, it never lets everybody in.
				*closeConnection = true;
				return Answer(id, false, Json(), "the remote control is not set up on the server (no token)");
			}

			std::string given_token = args.Get("token").AsString();
			bool match = given_token.size() == m_Token.size() && crypto::ConstantTimeEquals(given_token.data(), m_Token.data(), (uint32)m_Token.size());
			if (match)
			{
				session->Authenticated = true;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					m_FailedAttempts.erase(session->Id);
				}

				CAM_LOG_INFO("The client {0} logged in to the remote control.", session->Remote);
				Json result = Json::Object();
				result["authenticated"] = true;
				return Answer(id, true, result, "");
			}

			int failed = 0;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				failed = ++m_FailedAttempts[session->Id];
			}

			CAM_LOG_WARN("The client {0} sent a wrong token to the remote control ({1} of {2}).", session->Remote, failed, MAX_FAILED_ATTEMPTS);

			// Guessing is made slow.
			Core::SleepMS(300 * (uint32)failed);
			if (failed >= MAX_FAILED_ATTEMPTS)
			{
				*closeConnection = true;
			}

			return Answer(id, false, Json(), "wrong token");
		}

		if (!session->Authenticated)
		{
			return Answer(id, false, Json(), "not logged in: send {\"cmd\":\"auth\",\"args\":{\"token\":\"...\"}} first");
		}

		// ---- built in
		if (name == "ping")
		{
			Json result = Json::Object();
			result["pong"] = true;
			result["time_ms"] = (int64)Core::QueryMS();
			return Answer(id, true, result, "");
		}

		if (name == "help")
		{
			Json commands = Json::Array();
			Json built_in = Json::Object();
			built_in["cmd"] = "ping";
			built_in["description"] = "answers with pong, to see that the connection works";
			commands.Push(built_in);
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				for (const auto &entry : m_Commands)
				{
					Json item = Json::Object();
					item["cmd"] = entry.first;
					item["description"] = entry.second.Description;
					commands.Push(item);
				}
			}

			Json result = Json::Object();
			result["commands"] = commands;
			return Answer(id, true, result, "");
		}

		// ---- the commands of the program
		Handler handler;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			auto found = m_Commands.find(name);
			if (found == m_Commands.end())
			{
				return Answer(id, false, Json(), "unknown command '" + name + "' (the command \"help\" lists them)");
			}

			handler = found->second.Run;
		}

		Context context;
		context.SessionId = session->Id;
		context.Remote = session->Remote;
		context.IsLoopback = session->IsLoopback;

		Json result = Json::Object();
		std::string error;
		bool ok = false;
		try
		{
			ok = handler(args, context, &result, &error);
		}
		catch (const std::exception &exception)
		{
			ok = false;
			error = std::string("the command failed: ") + exception.what();
		}

		if (!ok && error.empty())
		{
			error = "the command failed";
		}

		return Answer(id, ok, result, error);
	}
}
