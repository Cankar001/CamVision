#pragma once

#include "Core.h"

namespace Core
{
	class Crypto
	{
	public:

		struct key_t
		{
			uint32 Size = 0;
			Byte Data[4096];
		};

		virtual ~Crypto() {}

		/// <summary>
		/// Cuts a key blob to its real size and clears everything behind it. Keys, which were created by older versions, were stored with a size of
		/// the whole buffer (4096 bytes), and contained uninitialized memory behind the actual key. Does nothing for keys, which are already exact.
		/// </summary>
		static void NormalizeKey(key_t *key);

		virtual bool GenKeys(key_t *pub, key_t *pri) = 0;

		// Generates a random token.
		virtual uint64 GenToken() = 0;

		// Returns true if the signing succeded.
		virtual bool SignSignature(Byte *sig, uint32 sig_bytes, Byte const *data, uint32 data_size, Byte const *pri_key, uint32 pri_key_bytes) = 0;

		// Returns true if the signature is valid for the given data.
		virtual bool TestSignature(void const *sig, uint32 sig_bytes, void const *src, uint32 src_bytes, Byte const *pub_key, uint32 pub_key_bytes) = 0;

		static Crypto *Create();
	};
}

