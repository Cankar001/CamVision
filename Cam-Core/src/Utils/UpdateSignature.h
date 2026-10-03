#pragma once

#include "Core/Core.h"

#include <cstring>
#include <vector>

namespace Core::utils
{
	/// <summary>
	/// Builds the data, which the update server signs and the update client verifies: a small header followed by the update file. The header holds
	/// the version and the size of the update, so the signature belongs to exactly this version. Without it, somebody who can send packets in the name of the
	/// server could offer an old signed update under a higher version number, as the signature of the file alone is valid for any version.
	/// The header also starts with a fixed text, so a signature made for another purpose with the same key is not accepted as an update.
	/// </summary>
	inline std::vector<Byte> BuildSignedUpdateData(uint32 version, const Byte *update, uint32 size)
	{
		static const char MAGIC[8] = { 'C', 'A', 'M', 'U', 'P', 'D', '0', '1' };

		std::vector<Byte> result(sizeof(MAGIC) + 8 + size);
		memcpy(result.data(), MAGIC, sizeof(MAGIC));

		// Little endian, whatever the machine is, so the server and the clients (a PC and a Raspberry Pi) build the same data.
		for (int i = 0; i < 4; ++i)
		{
			result[sizeof(MAGIC) + i] = (Byte)(version >> (8 * i));
			result[sizeof(MAGIC) + 4 + i] = (Byte)(size >> (8 * i));
		}

		if (size > 0)
		{
			memcpy(result.data() + sizeof(MAGIC) + 8, update, size);
		}

		return result;
	}
}
