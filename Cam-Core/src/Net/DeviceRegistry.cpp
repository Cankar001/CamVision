#include "DeviceRegistry.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Core
{
	namespace
	{
		std::string Trim(const std::string &text)
		{
			size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
			{
				return "";
			}

			size_t end = text.find_last_not_of(" \t\r\n");
			return text.substr(begin, end - begin + 1);
		}

		std::string LowerCase(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			return text;
		}

		int64 FileTime(const std::string &file)
		{
			std::error_code error;
			auto time = std::filesystem::last_write_time(file, error);
			return error ? 0 : (int64)time.time_since_epoch().count();
		}
	}

	const char *DeviceRoleName(DeviceRole role)
	{
		switch (role)
		{
			case DeviceRole::Camera: return "camera";
			case DeviceRole::Display: return "display";
			default: return "none";
		}
	}

	DeviceRole DeviceRoleFromName(const std::string &name)
	{
		std::string lower = LowerCase(Trim(name));
		if (lower == "camera")
		{
			return DeviceRole::Camera;
		}

		if (lower == "display")
		{
			return DeviceRole::Display;
		}

		return DeviceRole::None;
	}

	uint64 DeviceKeyId(const Byte key[crypto::KEY_BYTES])
	{
		// Not the key itself: a hash of the key with a fixed text in front of it.
		static const char PREFIX[] = "CamVision key id";
		Byte input[sizeof(PREFIX) - 1 + crypto::KEY_BYTES];
		memcpy(input, PREFIX, sizeof(PREFIX) - 1);
		memcpy(input + sizeof(PREFIX) - 1, key, crypto::KEY_BYTES);

		Byte hash[crypto::SHA256_BYTES];
		crypto::Sha256(input, (uint32)sizeof(input), hash);

		uint64 id = 0;
		memcpy(&id, hash, sizeof(id));
		return id;
	}

	std::string KeyIdToString(uint64 keyId)
	{
		return BytesToHex((const Byte *)&keyId, 4);
	}

	std::string BytesToHex(const Byte *data, uint32 bytes)
	{
		static const char DIGITS[] = "0123456789abcdef";
		std::string text;
		text.reserve(bytes * 2);
		for (uint32 i = 0; i < bytes; ++i)
		{
			text.push_back(DIGITS[data[i] >> 4]);
			text.push_back(DIGITS[data[i] & 15]);
		}

		return text;
	}

	bool HexToBytes(const std::string &input, Byte *out, uint32 bytes)
	{
		std::string text = Trim(input);
		if (text.size() != (size_t)bytes * 2)
		{
			return false;
		}

		auto digit = [](char c) -> int
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};

		for (uint32 i = 0; i < bytes; ++i)
		{
			int high = digit(text[i * 2]);
			int low = digit(text[i * 2 + 1]);
			if (high < 0 || low < 0)
			{
				return false;
			}

			out[i] = (Byte)((high << 4) | low);
		}

		return true;
	}

	DeviceRegistry::DeviceRegistry(const std::string &file)
		: m_File(file)
	{
	}

	bool DeviceRegistry::Load(std::string *error)
	{
		std::vector<Device> devices;
		std::string problems;

		std::ifstream stream(m_File);
		if (stream.is_open())
		{
			std::string line;
			int number = 0;
			while (std::getline(stream, line))
			{
				++number;
				std::string trimmed = Trim(line);
				if (trimmed.empty() || trimmed[0] == '#')
				{
					continue;
				}

				// role, key, name (the rest of the line)
				std::istringstream words(trimmed);
				std::string role_text, key_text;
				words >> role_text >> key_text;
				std::string name;
				std::getline(words, name);
				name = Trim(name);

				Device device;
				device.Role = DeviceRoleFromName(role_text);
				if (device.Role == DeviceRole::None || !HexToBytes(key_text, device.Key, crypto::KEY_BYTES) || name.empty())
				{
					problems += "line " + std::to_string(number) + " is not 'camera|display <64 hex characters> <name>'; ";
					continue;
				}

				device.Name = name;
				device.KeyId = DeviceKeyId(device.Key);
				devices.push_back(device);
			}
		}

		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Devices = devices;
			m_LoadedTime = FileTime(m_File);
		}

		if (!problems.empty() && error)
		{
			*error = m_File + ": " + problems;
		}

		return problems.empty();
	}

	bool DeviceRegistry::ReloadIfChanged()
	{
		int64 time = FileTime(m_File);
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (time == m_LoadedTime)
			{
				return false;
			}
		}

		Load(nullptr);
		return true;
	}

	bool DeviceRegistry::Find(uint64 keyId, Device *out) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (const Device &device : m_Devices)
		{
			if (device.KeyId == keyId)
			{
				*out = device;
				return true;
			}
		}

		return false;
	}

	std::vector<Device> DeviceRegistry::List() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Devices;
	}

	uint32 DeviceRegistry::Count() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return (uint32)m_Devices.size();
	}

	bool DeviceRegistry::IsValidName(const std::string &name, std::string *error) const
	{
		if (name.empty() || name.size() > MAX_NAME_LENGTH)
		{
			*error = "the name must have 1 to " + std::to_string(MAX_NAME_LENGTH) + " characters";
			return false;
		}

		for (unsigned char c : name)
		{
			if (c < 32 || c == 127)
			{
				*error = "the name must not contain control characters";
				return false;
			}
		}

		if (Trim(name) != name)
		{
			*error = "the name must not start or end with a space";
			return false;
		}

		for (const Device &device : m_Devices)
		{
			if (LowerCase(device.Name) == LowerCase(name))
			{
				*error = "there is a device with the name '" + name + "' already";
				return false;
			}
		}

		return true;
	}

	bool DeviceRegistry::Write(std::string *error) const
	{
		std::string text =
			"# The devices (cameras and displays), which are allowed to connect to the server. One line per device: role, key, name.\n"
			"# This file is a secret: whoever can read it can pretend to be one of the devices. Add devices with CamServer --add_device,\n"
			"# remove them with CamServer --remove_device (or delete the line). The server notices changes while it is running.\n";
		for (const Device &device : m_Devices)
		{
			text += std::string(DeviceRoleName(device.Role)) + " " + BytesToHex(device.Key, crypto::KEY_BYTES) + " " + device.Name + "\n";
		}

		// Write a new file and replace the old one with it, so a failure never leaves half of the file.
		std::string temporary = m_File + ".tmp";
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			if (!stream.is_open())
			{
				*error = "could not write " + temporary;
				return false;
			}

			stream << text;
			if (!stream.good())
			{
				*error = "could not write " + temporary;
				return false;
			}
		}

		std::error_code file_error;
#ifndef CAM_PLATFORM_WINDOWS
		// Only the owner may read the keys.
		std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, file_error);
#endif
		std::filesystem::rename(temporary, m_File, file_error);
		if (file_error)
		{
			// Windows does not replace a file with rename in all cases.
			std::filesystem::remove(m_File, file_error);
			file_error.clear();
			std::filesystem::rename(temporary, m_File, file_error);
		}

		if (file_error)
		{
			*error = "could not replace " + m_File + ": " + file_error.message();
			return false;
		}

		return true;
	}

	bool DeviceRegistry::Add(DeviceRole role, const std::string &name, Device *out, std::string *error)
	{
		std::string ignored;
		if (!error)
		{
			error = &ignored;
		}

		if (role == DeviceRole::None)
		{
			*error = "the role must be camera or display";
			return false;
		}

		// The file may have been changed by hand since the server read it.
		Load(nullptr);

		std::lock_guard<std::mutex> lock(m_Mutex);
		std::string trimmed = Trim(name);
		if (!IsValidName(trimmed, error))
		{
			return false;
		}

		Device device;
		device.Name = trimmed;
		device.Role = role;
		if (!crypto::RandomBytes(device.Key, crypto::KEY_BYTES))
		{
			*error = "the system could not make random numbers";
			return false;
		}

		device.KeyId = DeviceKeyId(device.Key);

		m_Devices.push_back(device);
		if (!Write(error))
		{
			m_Devices.pop_back();
			return false;
		}

		m_LoadedTime = FileTime(m_File);
		if (out)
		{
			*out = device;
		}

		return true;
	}

	bool DeviceRegistry::Remove(const std::string &name, std::string *error)
	{
		std::string ignored;
		if (!error)
		{
			error = &ignored;
		}

		Load(nullptr);

		std::lock_guard<std::mutex> lock(m_Mutex);
		std::string wanted = LowerCase(Trim(name));
		auto it = std::find_if(m_Devices.begin(), m_Devices.end(), [&](const Device &device) { return LowerCase(device.Name) == wanted; });
		if (it == m_Devices.end())
		{
			*error = "there is no device with the name '" + name + "'";
			return false;
		}

		Device removed = *it;
		m_Devices.erase(it);
		if (!Write(error))
		{
			m_Devices.push_back(removed);
			return false;
		}

		m_LoadedTime = FileTime(m_File);
		return true;
	}
}
