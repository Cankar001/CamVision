#pragma once

#include <memory>

#pragma warning( push )
#pragma warning( disable : 4996) // warning C4996: 'stdext::checked_array_iterator<T *>::iterator_category': warning STL4043: stdext::checked_array_iterator, stdext::unchecked_array_iterator, and related factory functions are non-Standard extensions and will be removed in the future. std::span (since C++20) and gsl::span can be used instead. You can define _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING or _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS to suppress this warning

#include <spdlog/logger.h>

namespace Core
{
	class Logger
	{
	public:

		Logger();
		virtual ~Logger();

		static Logger *Get();

		/// <summary>
		/// Turns all logging off: nothing is printed and no log file is created. Programs, whose output is read by other programs, use this.
		/// Must be called, before anything is logged for the first time.
		/// </summary>
		static void SetSilent(bool silent);

		std::shared_ptr<spdlog::logger> &GetLogger();
	};
}


#ifdef CAM_DEBUG
#define CAM_LOG_DEBUG(...) Core::Logger::Get()->GetLogger()->debug(__VA_ARGS__)
#define CAM_LOG_TRACE(...) Core::Logger::Get()->GetLogger()->trace(__VA_ARGS__)
#else
#define CAM_LOG_DEBUG(...)
#define CAM_LOG_TRACE(...)
#endif

#define CAM_LOG_INFO(...) Core::Logger::Get()->GetLogger()->info(__VA_ARGS__)
#define CAM_LOG_WARN(...) Core::Logger::Get()->GetLogger()->warn(__VA_ARGS__)
#define CAM_LOG_ERROR(...) Core::Logger::Get()->GetLogger()->error(__VA_ARGS__)
#define CAM_LOG_FATAL(...) Core::Logger::Get()->GetLogger()->critical(__VA_ARGS__)

#pragma warning( pop ) 
