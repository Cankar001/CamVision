#pragma once

#include "Core.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

namespace Core
{
	/// <summary>
	/// Simple key/value settings. Values come from a config file (one "key = value" per line, lines starting with '#' are comments)
	/// and can be overridden by command line arguments in the form --key=value (or just --key for boolean flags).
	/// Keys are case insensitive and '-' is the same as '_'. Arguments without leading "--" are ignored.
	/// The config file defaults to the one passed to the constructor and can be changed with --config=path.
	/// </summary>
	class Config
	{
	public:

		Config(int argc, char *argv[], const std::string &defaultFile)
		{
			std::unordered_map<std::string, std::string> args;
			for (int i = 1; i < argc; ++i)
			{
				std::string arg = argv[i];
				if (arg.rfind("--", 0) != 0)
				{
					continue;
				}

				arg = arg.substr(2);
				size_t separator = arg.find('=');
				if (separator == std::string::npos)
				{
					args[Normalize(arg)] = "true";
				}
				else
				{
					args[Normalize(arg.substr(0, separator))] = Trim(arg.substr(separator + 1));
				}
			}

			std::string file = defaultFile;
			auto config_arg = args.find("config");
			if (config_arg != args.end())
			{
				file = config_arg->second;
				args.erase(config_arg);
			}

			LoadFile(file);

			// Command line overrides the file.
			for (auto &entry : args)
			{
				m_Values[entry.first] = entry.second;
			}
		}

		std::string GetString(const std::string &key, const std::string &fallback) const
		{
			auto it = m_Values.find(Normalize(key));
			return it != m_Values.end() ? it->second : fallback;
		}

		int32 GetInt(const std::string &key, int32 fallback) const
		{
			auto it = m_Values.find(Normalize(key));
			if (it == m_Values.end())
			{
				return fallback;
			}

			char *end = nullptr;
			long value = strtol(it->second.c_str(), &end, 10);
			if (it->second.empty() || *end != 0)
			{
				CAM_LOG_ERROR("Setting '{0}' has an invalid number '{1}', using {2}.", key, it->second, fallback);
				return fallback;
			}

			return (int32)value;
		}

		bool GetBool(const std::string &key, bool fallback) const
		{
			auto it = m_Values.find(Normalize(key));
			if (it == m_Values.end())
			{
				return fallback;
			}

			std::string value = it->second;
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			if (value == "true" || value == "1" || value == "yes" || value == "on")
			{
				return true;
			}
			if (value == "false" || value == "0" || value == "no" || value == "off")
			{
				return false;
			}

			CAM_LOG_ERROR("Setting '{0}' has an invalid boolean '{1}', using {2}.", key, it->second, fallback);
			return fallback;
		}

		/// <summary>
		/// The file, which was actually loaded. Empty, if no config file was found.
		/// </summary>
		const std::string &LoadedFile() const { return m_LoadedFile; }

	private:

		static std::string Trim(const std::string &value)
		{
			size_t begin = value.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
			{
				return "";
			}

			size_t end = value.find_last_not_of(" \t\r\n");
			return value.substr(begin, end - begin + 1);
		}

		static std::string Normalize(const std::string &key)
		{
			std::string result = Trim(key);
			for (char &c : result)
			{
				c = (c == '-') ? '_' : (char)std::tolower((unsigned char)c);
			}

			return result;
		}

		void LoadFile(const std::string &file)
		{
			std::ifstream stream(file);
			if (!stream.is_open())
			{
				return;
			}

			m_LoadedFile = file;
			std::string line;
			while (std::getline(stream, line))
			{
				// Only whole lines can be comments, so values may contain a '#' (e.g. a name like "Client #1").
				std::string trimmed = Trim(line);
				if (trimmed.empty() || trimmed[0] == '#')
				{
					continue;
				}

				size_t separator = line.find('=');
				if (separator == std::string::npos)
				{
					continue;
				}

				std::string key = Normalize(line.substr(0, separator));
				if (!key.empty())
				{
					m_Values[key] = Trim(line.substr(separator + 1));
				}
			}
		}

	private:

		std::unordered_map<std::string, std::string> m_Values;
		std::string m_LoadedFile;
	};
}
