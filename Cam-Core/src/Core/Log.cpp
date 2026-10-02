#include "Log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace Core
{
	static std::shared_ptr<spdlog::logger> s_Logger;
	static bool s_Silent = false;

	void Logger::SetSilent(bool silent)
	{
		s_Silent = silent;
	}

	Logger::Logger()
	{
		if (s_Silent)
		{
			// No sinks: nothing is printed and the log file is not touched.
			s_Logger = std::make_shared<spdlog::logger>("App");
			s_Logger->set_level(spdlog::level::off);
			return;
		}

		spdlog::sink_ptr console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		console_sink->set_pattern("%^[%T] %n: %v%$");
		
		spdlog::sink_ptr file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/app.log", true);
		file_sink->set_pattern("%^[%T] %n: %v%$");

		std::vector<spdlog::sink_ptr> sinks;
		sinks.push_back(console_sink);
		sinks.push_back(file_sink);

		s_Logger = std::make_shared<spdlog::logger>("App", sinks.begin(), sinks.end());
		s_Logger->set_level(spdlog::level::trace);
	}

	Logger::~Logger()
	{
		s_Logger.reset();

		spdlog::drop_all();
	}

	Logger *Logger::Get()
	{
		static Logger instance;
		return &instance;
	}

	std::shared_ptr<spdlog::logger> &Logger::GetLogger()
	{
		return s_Logger;
	}
}
