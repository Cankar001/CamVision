#include "Crypto.h"

#ifdef CAM_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsCrypto.h"
#elif CAM_PLATFORM_LINUX
#include "Platform/Linux/LinuxCrypto.h"
#endif

#include <string.h>

namespace Core
{
	void Crypto::NormalizeKey(key_t *key)
	{
		// Key blob header (20 bytes): type, version, reserved (2), algorithm (4), magic (4), bit length (4), public exponent (4).
		const uint32 header_size = 20;
		if (!key || key->Size < header_size || key->Size > sizeof(key->Data))
		{
			return;
		}

		Byte type = key->Data[0];
		uint32 bits = (uint32)key->Data[12] | ((uint32)key->Data[13] << 8) | ((uint32)key->Data[14] << 16) | ((uint32)key->Data[15] << 24);
		if (bits < 512 || bits > 16384 || bits % 16 != 0)
		{
			return;
		}

		uint32 needed = header_size + bits / 8;	// public key: the modulus
		if (type == 0x07)
		{
			// private key: modulus, 5 numbers of half the size (primes, exponents, coefficient) and the private exponent
			needed += 5 * (bits / 16) + bits / 8;
		}
		else if (type != 0x06)
		{
			return;
		}

		if (needed < key->Size)
		{
			memset(key->Data + needed, 0, sizeof(key->Data) - needed);
			key->Size = needed;
		}
	}

	Crypto *Crypto::Create()
	{
#ifdef CAM_PLATFORM_WINDOWS
		return new WindowsCrypto();
#elif CAM_PLATFORM_LINUX
		return new LinuxCrypto();
#endif
	}
}

