#include "Core/CryptoPrimitives.h"

#ifdef CAM_PLATFORM_LINUX

#include <cstring>
#include <memory>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace Core::crypto
{
	namespace
	{
		struct ContextDeleter
		{
			void operator()(EVP_CIPHER_CTX *context) const { EVP_CIPHER_CTX_free(context); }
		};

		using Context = std::unique_ptr<EVP_CIPHER_CTX, ContextDeleter>;
	}

	bool RandomBytes(void *dst, uint32 bytes)
	{
		return RAND_bytes((unsigned char *)dst, (int)bytes) == 1;
	}

	void Sha256(const void *data, uint32 bytes, Byte out[SHA256_BYTES])
	{
		SHA256((const unsigned char *)data, bytes, out);
	}

	void Sha1(const void *data, uint32 bytes, Byte out[SHA1_BYTES])
	{
		SHA1((const unsigned char *)data, bytes, out);
	}

	void HmacSha256(const void *key, uint32 keyBytes, const void *data, uint32 bytes, Byte out[SHA256_BYTES])
	{
		unsigned int length = SHA256_BYTES;
		memset(out, 0, SHA256_BYTES);

		// An empty key would be refused by some versions, a key of zero bytes is the same for HMAC.
		Byte zero = 0;
		HMAC(EVP_sha256(), keyBytes ? key : &zero, keyBytes ? (int)keyBytes : 1, (const unsigned char *)data, bytes, out, &length);
	}

	bool AesGcmSeal(const Byte key[KEY_BYTES], const Byte nonce[NONCE_BYTES], const void *aad, uint32 aadBytes, const void *plain, uint32 bytes, void *ciphertext, Byte tag[TAG_BYTES])
	{
		Context context(EVP_CIPHER_CTX_new());
		if (!context)
		{
			return false;
		}

		int length = 0;
		if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
			|| EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, (int)NONCE_BYTES, nullptr) != 1
			|| EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key, nonce) != 1)
		{
			return false;
		}

		if (aadBytes > 0 && EVP_EncryptUpdate(context.get(), nullptr, &length, (const unsigned char *)aad, (int)aadBytes) != 1)
		{
			return false;
		}

		if (bytes > 0 && EVP_EncryptUpdate(context.get(), (unsigned char *)ciphertext, &length, (const unsigned char *)plain, (int)bytes) != 1)
		{
			return false;
		}

		unsigned char final_block[16];
		if (EVP_EncryptFinal_ex(context.get(), final_block, &length) != 1)
		{
			return false;
		}

		return EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, (int)TAG_BYTES, tag) == 1;
	}

	bool AesGcmOpen(const Byte key[KEY_BYTES], const Byte nonce[NONCE_BYTES], const void *aad, uint32 aadBytes, const void *ciphertext, uint32 bytes, const Byte tag[TAG_BYTES], void *plain)
	{
		Context context(EVP_CIPHER_CTX_new());
		if (!context)
		{
			return false;
		}

		int length = 0;
		if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
			|| EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, (int)NONCE_BYTES, nullptr) != 1
			|| EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key, nonce) != 1)
		{
			return false;
		}

		if (aadBytes > 0 && EVP_DecryptUpdate(context.get(), nullptr, &length, (const unsigned char *)aad, (int)aadBytes) != 1)
		{
			return false;
		}

		if (bytes > 0 && EVP_DecryptUpdate(context.get(), (unsigned char *)plain, &length, (const unsigned char *)ciphertext, (int)bytes) != 1)
		{
			return false;
		}

		Byte tag_copy[TAG_BYTES];
		memcpy(tag_copy, tag, TAG_BYTES);
		if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, (int)TAG_BYTES, tag_copy) != 1)
		{
			return false;
		}

		unsigned char final_block[16];
		return EVP_DecryptFinal_ex(context.get(), final_block, &length) == 1;
	}
}

#endif // CAM_PLATFORM_LINUX
