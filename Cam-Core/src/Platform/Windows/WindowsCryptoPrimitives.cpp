#include "Core/CryptoPrimitives.h"

#ifdef CAM_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <bcrypt.h>

#include <cstring>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")

namespace Core::crypto
{
	namespace
	{
		// The algorithm providers are opened once and used from all threads (this is allowed, they only describe the algorithm).
		struct Providers
		{
			BCRYPT_ALG_HANDLE Sha256 = nullptr;
			BCRYPT_ALG_HANDLE Hmac = nullptr;
			BCRYPT_ALG_HANDLE Aes = nullptr;

			Providers()
			{
				BCryptOpenAlgorithmProvider(&Sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
				BCryptOpenAlgorithmProvider(&Hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
				if (BCryptOpenAlgorithmProvider(&Aes, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
				{
					BCryptSetProperty(Aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
				}
			}
		};

		Providers &GetProviders()
		{
			static Providers providers;
			return providers;
		}

		bool Success(NTSTATUS status)
		{
			return status >= 0;
		}
	}

	bool RandomBytes(void *dst, uint32 bytes)
	{
		return Success(BCryptGenRandom(nullptr, (PUCHAR)dst, bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
	}

	void Sha256(const void *data, uint32 bytes, Byte out[SHA256_BYTES])
	{
		memset(out, 0, SHA256_BYTES);
		BCryptHash(GetProviders().Sha256, nullptr, 0, (PUCHAR)data, bytes, out, SHA256_BYTES);
	}

	void HmacSha256(const void *key, uint32 keyBytes, const void *data, uint32 bytes, Byte out[SHA256_BYTES])
	{
		memset(out, 0, SHA256_BYTES);
		BCryptHash(GetProviders().Hmac, (PUCHAR)key, keyBytes, (PUCHAR)data, bytes, out, SHA256_BYTES);
	}

	bool AesGcmSeal(const Byte key[KEY_BYTES], const Byte nonce[NONCE_BYTES], const void *aad, uint32 aadBytes, const void *plain, uint32 bytes, void *ciphertext, Byte tag[TAG_BYTES])
	{
		BCRYPT_KEY_HANDLE handle = nullptr;
		if (!Success(BCryptGenerateSymmetricKey(GetProviders().Aes, &handle, nullptr, 0, (PUCHAR)key, KEY_BYTES, 0)))
		{
			return false;
		}

		BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
		BCRYPT_INIT_AUTH_MODE_INFO(info);
		info.pbNonce = (PUCHAR)nonce;
		info.cbNonce = NONCE_BYTES;
		info.pbAuthData = aadBytes ? (PUCHAR)aad : nullptr;
		info.cbAuthData = aadBytes;
		info.pbTag = tag;
		info.cbTag = TAG_BYTES;

		ULONG written = 0;
		Byte empty = 0;
		NTSTATUS status = BCryptEncrypt(handle, bytes ? (PUCHAR)plain : &empty, bytes, &info, nullptr, 0, bytes ? (PUCHAR)ciphertext : &empty, bytes, &written, 0);
		BCryptDestroyKey(handle);
		return Success(status);
	}

	bool AesGcmOpen(const Byte key[KEY_BYTES], const Byte nonce[NONCE_BYTES], const void *aad, uint32 aadBytes, const void *ciphertext, uint32 bytes, const Byte tag[TAG_BYTES], void *plain)
	{
		BCRYPT_KEY_HANDLE handle = nullptr;
		if (!Success(BCryptGenerateSymmetricKey(GetProviders().Aes, &handle, nullptr, 0, (PUCHAR)key, KEY_BYTES, 0)))
		{
			return false;
		}

		Byte tag_copy[TAG_BYTES];
		memcpy(tag_copy, tag, TAG_BYTES);

		BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
		BCRYPT_INIT_AUTH_MODE_INFO(info);
		info.pbNonce = (PUCHAR)nonce;
		info.cbNonce = NONCE_BYTES;
		info.pbAuthData = aadBytes ? (PUCHAR)aad : nullptr;
		info.cbAuthData = aadBytes;
		info.pbTag = tag_copy;
		info.cbTag = TAG_BYTES;

		ULONG written = 0;
		Byte empty = 0;
		NTSTATUS status = BCryptDecrypt(handle, bytes ? (PUCHAR)ciphertext : &empty, bytes, &info, nullptr, 0, bytes ? (PUCHAR)plain : &empty, bytes, &written, 0);
		BCryptDestroyKey(handle);
		return Success(status);
	}
}

#endif // CAM_PLATFORM_WINDOWS
