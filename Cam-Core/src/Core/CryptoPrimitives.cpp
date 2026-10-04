#include "CryptoPrimitives.h"

#include <algorithm>
#include <cstring>

namespace Core::crypto
{
	bool ConstantTimeEquals(const void *a, const void *b, uint32 bytes)
	{
		const Byte *x = (const Byte *)a;
		const Byte *y = (const Byte *)b;
		Byte difference = 0;
		for (uint32 i = 0; i < bytes; ++i)
		{
			difference |= (Byte)(x[i] ^ y[i]);
		}

		return difference == 0;
	}

	void HkdfSha256(const void *salt, uint32 saltBytes, const void *secret, uint32 secretBytes, const void *info, uint32 infoBytes, Byte *out, uint32 outBytes)
	{
		// Extract: one pseudo random key out of the secret and the salt. An empty salt is 32 zero bytes.
		Byte zero_salt[SHA256_BYTES] = {};
		Byte pseudo_random_key[SHA256_BYTES];
		if (saltBytes == 0)
		{
			salt = zero_salt;
			saltBytes = sizeof(zero_salt);
		}

		HmacSha256(salt, saltBytes, secret, secretBytes, pseudo_random_key);

		// Expand: block after block, every block depends on the one before.
		Byte block[SHA256_BYTES] = {};
		uint32 block_bytes = 0;
		uint32 written = 0;
		for (Byte counter = 1; written < outBytes; ++counter)
		{
			// HMAC(pseudo_random_key, previous block | info | counter)
			Byte input[SHA256_BYTES + 256 + 1];
			uint32 input_bytes = 0;
			memcpy(input, block, block_bytes);
			input_bytes += block_bytes;
			uint32 info_used = std::min<uint32>(infoBytes, 256);
			memcpy(input + input_bytes, info, info_used);
			input_bytes += info_used;
			input[input_bytes++] = counter;

			HmacSha256(pseudo_random_key, sizeof(pseudo_random_key), input, input_bytes, block);
			block_bytes = SHA256_BYTES;

			uint32 take = std::min<uint32>(SHA256_BYTES, outBytes - written);
			memcpy(out + written, block, take);
			written += take;
		}
	}
}
