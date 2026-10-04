#pragma once

#include "Core/Core.h"
#include "Utils/Json.h"
#include "WebSocket.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace Core
{
	/// <summary>
	/// The commands, which are sent over the WebSocket, as JSON text messages.
	///
	/// A request:   {"id": 7, "cmd": "save", "args": {"camera": "Front door", "minutes": 5}}
	/// The answer:  {"id": 7, "ok": true, "result": { ... }}      or      {"id": 7, "ok": false, "error": "what went wrong"}
	/// The "id" (any JSON value) is optional and comes back unchanged, so a client can match the answers to its requests. "args" is optional.
	///
	/// The first command of every connection must be {"cmd": "auth", "args": {"token": "..."}}. Nothing else is done before. Three wrong tokens end the connection.
	/// Built in: auth, ping ({"pong": true, "time_ms": ...}), help (the list of all commands).
	/// </summary>
	class CommandDispatcher
	{
	public:

		/// <summary>
		/// What a command handler knows about the client, which sent it.
		/// </summary>
		struct Context
		{
			uint64 SessionId = 0;
			std::string Remote;

			/// <summary>
			/// True, if the client is a program on this computer. Commands, which hand out secrets, can insist on that.
			/// </summary>
			bool IsLoopback = false;
		};

		/// <summary>
		/// Does the command. Fill the result, or return false with an error text. Is called on the thread of the connection, so a long command only holds up
		/// the one client.
		/// </summary>
		using Handler = std::function<bool(const Json &args, const Context &context, Json *result, std::string *error)>;

		/// <summary>
		/// The secret, which a client has to send with "auth". It must not be empty (that would let everybody in), the dispatcher refuses everything then.
		/// </summary>
		explicit CommandDispatcher(const std::string &token);

		/// <summary>
		/// Adds a command. The name is the "cmd" of the requests. The description is shown by "help".
		/// </summary>
		void Register(const std::string &name, const std::string &description, Handler handler);

		/// <summary>
		/// Handles one message of a client: returns the answer (JSON text), and tells in session->Authenticated, if the client is allowed to give commands.
		/// </summary>
		std::string Process(WebSocketSession *session, const std::string &message, bool *closeConnection);

		/// <summary>
		/// A message for the WebSocket server: Process() as its handler.
		/// </summary>
		WebSocketServer::MessageHandler AsMessageHandler();

		/// <summary>
		/// An event, which the server sends to all clients: {"event": name, "data": data}.
		/// </summary>
		static std::string MakeEvent(const std::string &name, const Json &data);

	private:

		struct Command
		{
			std::string Description;
			Handler Run;
		};

		std::string Answer(const Json &id, bool ok, const Json &resultOrNull, const std::string &error) const;

		std::string m_Token;
		std::map<std::string, Command> m_Commands;
		std::map<uint64, int> m_FailedAttempts;
		std::mutex m_Mutex;
	};
}
