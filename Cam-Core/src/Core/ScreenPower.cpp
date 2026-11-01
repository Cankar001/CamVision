#include "ScreenPower.h"

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#include "Log.h"

namespace Core
{
	ScreenPower::Settings ScreenPower::Settings::FromConfig(const Config &config)
	{
		Settings settings;
		settings.Auto = config.GetString("screen_mode", "on") == "auto";
		settings.TimeoutSeconds = (uint32)std::max(config.GetInt("screen_timeout", settings.TimeoutSeconds), 1);
		settings.OnCommand = config.GetString("screen_on_command", settings.OnCommand);
		settings.OffCommand = config.GetString("screen_off_command", settings.OffCommand);
		return settings;
	}

	void ScreenPower::Start()
	{
		if (!m_Settings.Auto)
		{
			return;
		}

		// The screen is on at the start, and stays on for the timeout.
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Started = true;
		m_LastWakeMS = QueryMS();
		m_ScreenOn = false;
		Set(true);
	}

	void ScreenPower::Update()
	{
		if (!m_Settings.Auto)
		{
			return;
		}

		int64 wake_ms = m_LastWakeMS;
		bool wanted = wake_ms != 0 && QueryMS() - wake_ms < (int64)m_Settings.TimeoutSeconds * 1000;

		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Started && wanted != m_ScreenOn)
		{
			Set(wanted);
		}
	}

	void ScreenPower::Stop()
	{
		if (!m_Settings.Auto)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Started)
		{
			Set(true);
			m_Started = false;
		}
	}

	void ScreenPower::Set(bool on)
	{
		if (on == m_ScreenOn)
		{
			return;
		}

		m_ScreenOn = on;
		if (on)
		{
			CAM_LOG_INFO("Movement (or touch): switching the screen on.");
		}
		else
		{
			CAM_LOG_INFO("No movement for {} seconds: switching the screen off.", m_Settings.TimeoutSeconds);
		}

		const std::string &custom = on ? m_Settings.OnCommand : m_Settings.OffCommand;
		if (!custom.empty())
		{
			if (std::system(custom.c_str()) != 0)
			{
				CAM_LOG_WARN("The command to switch the screen {0} failed: {1}", on ? "on" : "off", custom);
			}
			return;
		}

#ifdef _WIN32
		// -1: on, 2: off. The execution state keeps the system from putting the screen to sleep by itself, while it should be on.
		SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
		SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, on ? -1 : 2, SMTO_ABORTIFHUNG, 1000, nullptr);
		if (on)
		{
			// Moving the mouse by nothing wakes up a screen, which the message alone did not.
			mouse_event(MOUSEEVENTF_MOVE, 0, 0, 0, 0);
		}
#else
		// X11 (also on the Raspberry Pi desktop): with the timeouts at 0, the system never sleeps by itself, and "force" switches the screen on or off.
		// Older Raspberry Pi systems without X11 power management know vcgencmd.
		const char *command = on ?
			"xset s off s noblank +dpms dpms 0 0 0 dpms force on >/dev/null 2>&1 || vcgencmd display_power 1 >/dev/null 2>&1" :
			"xset +dpms dpms 0 0 0 dpms force off >/dev/null 2>&1 || vcgencmd display_power 0 >/dev/null 2>&1";
		if (std::system(command) != 0)
		{
			CAM_LOG_WARN("Could not switch the screen {}. Is xset (or vcgencmd) installed? On Wayland, set screen_on_command and screen_off_command.", on ? "on" : "off");
		}
#endif
	}
}
