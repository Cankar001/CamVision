#pragma once

#include "Core/Core.h"
#include "Core/CryptoPrimitives.h"

#include <mutex>
#include <string>
#include <vector>

namespace Core
{
	/// <summary>
	/// What a device is allowed to do. A camera sends frames, a display receives them. A key of a display cannot be used to send frames, and the other way around.
	/// </summary>
	enum class DeviceRole : uint8
	{
		None = 0,
		Camera = 1,
		Display = 2
	};

	const char *DeviceRoleName(DeviceRole role);
	DeviceRole DeviceRoleFromName(const std::string &name);

	/// <summary>
	/// A device, which is allowed to connect: a camera or a display with its own key. The key is a secret, which is only known to the device and to the server.
	/// </summary>
	struct Device
	{
		std::string Name;
		DeviceRole Role = DeviceRole::None;
		Byte Key[crypto::KEY_BYTES] = {};

		// Identifies the key without telling it (a device says with this, which key it uses).
		uint64 KeyId = 0;
	};

	/// <summary>
	/// The id of a key: the first bytes of a hash of the key (one way, it tells nothing about the key).
	/// </summary>
	uint64 DeviceKeyId(const Byte key[crypto::KEY_BYTES]);

	/// <summary>
	/// The id of a key as short text (for logs and lists, 8 hex characters).
	/// </summary>
	std::string KeyIdToString(uint64 keyId);

	std::string BytesToHex(const Byte *data, uint32 bytes);

	/// <summary>
	/// Reads exactly `bytes` bytes from the hex text (spaces at the ends are fine). Returns false for anything else.
	/// </summary>
	bool HexToBytes(const std::string &text, Byte *out, uint32 bytes);

	/// <summary>
	/// The devices, which may connect to the server, in a file (devices.cfg of the server). One line per device:
	///   camera  3f9a...64 hex characters...  Front door
	///   display 71c0...64 hex characters...  Living room
	/// The role, the key and then the name (up to the end of the line). Lines starting with # are comments. The file is a secret: everybody who can read it
	/// can pretend to be a device.
	/// </summary>
	class DeviceRegistry
	{
	public:

		explicit DeviceRegistry(const std::string &file);

		/// <summary>
		/// Reads the file. A file, which does not exist, is an empty list (no device may connect). Returns false, if the file exists but has errors, the
		/// error says where. The valid lines are used anyway.
		/// </summary>
		bool Load(std::string *error);

		/// <summary>
		/// Reads the file again, if it changed since it was read (devices can be added or removed, while the server is running). Returns true, if it was read.
		/// </summary>
		bool ReloadIfChanged();

		bool Find(uint64 keyId, Device *out) const;
		std::vector<Device> List() const;
		uint32 Count() const;

		/// <summary>
		/// Makes a new device with a new random key and stores it in the file. The name must be new.
		/// </summary>
		bool Add(DeviceRole role, const std::string &name, Device *out, std::string *error);

		/// <summary>
		/// Removes the device with this name from the file. Its key does not work anymore, as soon as the server read the file again.
		/// </summary>
		bool Remove(const std::string &name, std::string *error);

		const std::string &File() const { return m_File; }

		/// <summary>
		/// The longest possible name of a device (the names are sent to the server, which has a limit for them).
		/// </summary>
		static constexpr uint32 MAX_NAME_LENGTH = 31;

	private:

		bool Write(std::string *error) const;
		bool IsValidName(const std::string &name, std::string *error) const;

	private:

		std::string m_File;
		mutable std::mutex m_Mutex;
		std::vector<Device> m_Devices;
		int64 m_LoadedTime = 0;
	};
}
