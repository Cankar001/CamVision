#pragma once

#include "Core.h"
#include "Config.h"
#include "Timer.h"

#include <atomic>
#include <mutex>
#include <string>

namespace Core
{
	/// <summary>
	/// Switches the screen of this computer on and off by itself ("auto mode"): the screen is on, while something happened lately (Wake() was called, for
	/// example because the server reported movement), and off after the timeout. In auto mode the system's own sleep timers are turned off, this class decides.
	/// </summary>
	class ScreenPower
	{
	public:

		struct Settings
		{
			/// <summary>
			/// false: the screen is not touched at all (the system decides when it goes to sleep).
			/// </summary>
			bool Auto = false;

			/// <summary>
			/// How long the screen stays on after the last Wake().
			/// </summary>
			uint32 TimeoutSeconds = 60;

			/// <summary>
			/// The commands (run by the shell), which switch the screen on and off. Empty uses the default of the system: xset (or vcgencmd on a Raspberry Pi)
			/// on Linux, the monitor power message on Windows. Needed for example on Wayland (wlr-randr --output HDMI-A-1 --off).
			/// </summary>
			std::string OnCommand;
			std::string OffCommand;

			/// <summary>
			/// Reads screen_mode ("on" or "auto"), screen_timeout, screen_on_command and screen_off_command.
			/// </summary>
			static Settings FromConfig(const Config &config);
		};

		explicit ScreenPower(const Settings &settings) : m_Settings(settings) {}

		bool IsAuto() const { return m_Settings.Auto; }

		/// <summary>
		/// Switches the screen on and starts the timeout. Does nothing without auto mode.
		/// </summary>
		void Start();

		/// <summary>
		/// Something happened (movement, touch): the screen is switched on at the next Update(), and stays on for the timeout. Safe to call from any thread.
		/// </summary>
		void Wake() { m_LastWakeMS = QueryMS(); }

		/// <summary>
		/// Switches the screen on or off, as the last Wake() requires. Call it regularly (it is cheap, if nothing changes). Does nothing without auto mode.
		/// </summary>
		void Update();

		/// <summary>
		/// Leaves the screen on, because nobody is there anymore to switch it on again.
		/// </summary>
		void Stop();

	private:

		void Set(bool on);

	private:

		Settings m_Settings;
		std::atomic<int64> m_LastWakeMS{ 0 };
		std::mutex m_Mutex;
		bool m_ScreenOn = true;
		bool m_Started = false;
	};
}
