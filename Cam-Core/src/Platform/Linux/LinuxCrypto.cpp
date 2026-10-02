#include "LinuxCrypto.h"

#ifdef CAM_PLATFORM_LINUX

// The classic RSA functions are deprecated since OpenSSL 3.0, but they exist in all versions (1.1.x and 3.x) and keep this file independent of the installed version.
#define OPENSSL_SUPPRESS_DEPRECATED

#include <algorithm>
#include <iostream>
#include <vector>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

// The keys and signatures use exactly the format of the Windows implementation (CryptoAPI), so an update server running on Windows and a client
// running on Linux (or the other way around) understand each other:
//
//   public key blob : BLOBHEADER | RSAPUBKEY | modulus
//   private key blob: BLOBHEADER | RSAPUBKEY | modulus | prime1 | prime2 | exponent1 | exponent2 | coefficient | privateExponent
//   signature       : PKCS#1 v1.5 signature over SHA-256, but with the bytes in little endian order
//
// All numbers inside the blobs are little endian, modulus and privateExponent are bitlen / 8 bytes long, all others bitlen / 16.

namespace Core
{
	namespace
	{
		constexpr Byte BLOB_TYPE_PUBLIC = 0x06;
		constexpr Byte BLOB_TYPE_PRIVATE = 0x07;
		constexpr Byte BLOB_VERSION = 0x02;
		constexpr uint32 ALG_RSA_KEYX = 0x0000A400;		// CALG_RSA_KEYX, as used for AT_KEYEXCHANGE keys.
		constexpr uint32 MAGIC_PUBLIC = 0x31415352;		// "RSA1"
		constexpr uint32 MAGIC_PRIVATE = 0x32415352;	// "RSA2"
		constexpr uint32 KEY_BITS = 4096;
		constexpr uint32 BLOB_HEADER_SIZE = 8 + 12;		// BLOBHEADER + RSAPUBKEY

		void WriteU32(Byte *dst, uint32 value)
		{
			dst[0] = (Byte)(value);
			dst[1] = (Byte)(value >> 8);
			dst[2] = (Byte)(value >> 16);
			dst[3] = (Byte)(value >> 24);
		}

		uint32 ReadU32(Byte const *src)
		{
			return (uint32)src[0] | ((uint32)src[1] << 8) | ((uint32)src[2] << 16) | ((uint32)src[3] << 24);
		}

		// Writes a number as little endian with a fixed length. Returns the position behind it, or nullptr if it does not fit.
		Byte *WriteNumber(Byte *dst, Byte const *end, const BIGNUM *number, uint32 bytes)
		{
			if (!number || (uint32)(end - dst) < bytes || BN_bn2lebinpad(number, dst, (int)bytes) < 0)
			{
				return nullptr;
			}

			return dst + bytes;
		}

		void WriteHeader(Byte *dst, Byte type, uint32 magic, uint32 bits, uint32 exponent)
		{
			dst[0] = type;
			dst[1] = BLOB_VERSION;
			dst[2] = 0;
			dst[3] = 0;
			WriteU32(dst + 4, ALG_RSA_KEYX);
			WriteU32(dst + 8, magic);
			WriteU32(dst + 12, bits);
			WriteU32(dst + 16, exponent);
		}

		BIGNUM *ReadNumber(Byte const *&src, Byte const *end, uint32 bytes)
		{
			if ((uint32)(end - src) < bytes)
			{
				return nullptr;
			}

			BIGNUM *number = BN_lebin2bn(src, (int)bytes, nullptr);
			src += bytes;
			return number;
		}

		// Builds an RSA key from a Windows key blob. Returns nullptr, if the blob is malformed or not of the expected type.
		RSA *ImportKey(Byte const *blob, uint32 blob_bytes, bool expectPrivate)
		{
			if (!blob || blob_bytes < BLOB_HEADER_SIZE)
			{
				return nullptr;
			}

			Byte expectedType = expectPrivate ? BLOB_TYPE_PRIVATE : BLOB_TYPE_PUBLIC;
			uint32 expectedMagic = expectPrivate ? MAGIC_PRIVATE : MAGIC_PUBLIC;
			if (blob[0] != expectedType || blob[1] != BLOB_VERSION || ReadU32(blob + 8) != expectedMagic)
			{
				return nullptr;
			}

			uint32 bits = ReadU32(blob + 12);
			uint32 exponent = ReadU32(blob + 16);
			if (bits < 512 || bits > 16384 || bits % 16 != 0)
			{
				return nullptr;
			}

			Byte const *ptr = blob + BLOB_HEADER_SIZE;
			Byte const *end = blob + blob_bytes;

			BIGNUM *n = ReadNumber(ptr, end, bits / 8);
			BIGNUM *e = BN_new();
			BIGNUM *p = nullptr, *q = nullptr, *dp = nullptr, *dq = nullptr, *iqmp = nullptr, *d = nullptr;
			if (e)
			{
				BN_set_word(e, exponent);
			}

			if (expectPrivate)
			{
				p = ReadNumber(ptr, end, bits / 16);
				q = ReadNumber(ptr, end, bits / 16);
				dp = ReadNumber(ptr, end, bits / 16);
				dq = ReadNumber(ptr, end, bits / 16);
				iqmp = ReadNumber(ptr, end, bits / 16);
				d = ReadNumber(ptr, end, bits / 8);
			}

			RSA *rsa = RSA_new();
			bool complete = n && e && rsa && (!expectPrivate || (p && q && dp && dq && iqmp && d));
			if (complete)
			{
				// The RSA object takes the ownership of the numbers, if the calls succeed.
				complete = RSA_set0_key(rsa, n, e, d) == 1;
				if (complete)
				{
					n = e = d = nullptr;
					if (expectPrivate)
					{
						complete = RSA_set0_factors(rsa, p, q) == 1;
						if (complete)
						{
							p = q = nullptr;
							complete = RSA_set0_crt_params(rsa, dp, dq, iqmp) == 1;
							if (complete)
							{
								dp = dq = iqmp = nullptr;
							}
						}
					}
				}
			}

			BN_free(n); BN_free(e); BN_free(p); BN_free(q); BN_free(dp); BN_free(dq); BN_free(iqmp); BN_free(d);

			if (!complete)
			{
				RSA_free(rsa);
				return nullptr;
			}

			return rsa;
		}

		void HashSha256(void const *data, uint32 bytes, Byte *out_digest)
		{
			SHA256((Byte const *)data, bytes, out_digest);
		}
	}

	LinuxCrypto::LinuxCrypto()
	{
	}

	LinuxCrypto::~LinuxCrypto()
	{
	}

	bool LinuxCrypto::GenKeys(key_t *pub, key_t *pri)
	{
		if (!pub || !pri)
		{
			return false;
		}

		const uint32 exponent = 0x10001;
		bool success = false;

		RSA *rsa = RSA_new();
		BIGNUM *e = BN_new();
		if (rsa && e && BN_set_word(e, exponent) == 1 && RSA_generate_key_ex(rsa, (int)KEY_BITS, e, nullptr) == 1)
		{
			const BIGNUM *n, *ee, *d, *p, *q, *dp, *dq, *iqmp;
			RSA_get0_key(rsa, &n, &ee, &d);
			RSA_get0_factors(rsa, &p, &q);
			RSA_get0_crt_params(rsa, &dp, &dq, &iqmp);

			// public key
			Byte *pub_end = pub->Data + sizeof(pub->Data);
			WriteHeader(pub->Data, BLOB_TYPE_PUBLIC, MAGIC_PUBLIC, KEY_BITS, exponent);
			Byte *ptr = WriteNumber(pub->Data + BLOB_HEADER_SIZE, pub_end, n, KEY_BITS / 8);

			// private key
			Byte *pri_end = pri->Data + sizeof(pri->Data);
			WriteHeader(pri->Data, BLOB_TYPE_PRIVATE, MAGIC_PRIVATE, KEY_BITS, exponent);
			Byte *pri_ptr = pri->Data + BLOB_HEADER_SIZE;
			pri_ptr = pri_ptr ? WriteNumber(pri_ptr, pri_end, n, KEY_BITS / 8) : nullptr;
			pri_ptr = pri_ptr ? WriteNumber(pri_ptr, pri_end, p, KEY_BITS / 16) : nullptr;
			pri_ptr = pri_ptr ? WriteNumber(pri_ptr, pri_end, q, KEY_BITS / 16) : nullptr;
			pri_ptr = pri_ptr ? WriteNumber(pri_ptr, pri_end, dp, KEY_BITS / 16) : nullptr;
			pri_ptr = pri_ptr ? WriteNumber(pri_ptr, pri_end, dq, KEY_BITS / 16) : nullptr;
			pri_ptr = pri_ptr ? WriteNumber(pri_ptr, pri_end, iqmp, KEY_BITS / 16) : nullptr;
			pri_ptr = pri_ptr ? WriteNumber(pri_ptr, pri_end, d, KEY_BITS / 8) : nullptr;

			if (ptr && pri_ptr)
			{
				pub->Size = (uint32)(ptr - pub->Data);
				pri->Size = (uint32)(pri_ptr - pri->Data);
				success = true;
			}
			else
			{
				std::cerr << "Could not export the generated keys!" << std::endl;
			}
		}
		else
		{
			std::cerr << "Could not generate the RSA key pair!" << std::endl;
		}

		BN_free(e);
		RSA_free(rsa);
		return success;
	}

	uint64 LinuxCrypto::GenToken()
	{
		uint64 result = 0;
		if (RAND_bytes((unsigned char *)&result, sizeof(result)) != 1)
		{
			return 0;
		}

		return result;
	}

	bool LinuxCrypto::SignSignature(Byte *sig, uint32 sig_bytes, Byte const *data, uint32 data_size, Byte const *pri_key, uint32 pri_key_bytes)
	{
		if (!sig || !data)
		{
			return false;
		}

		RSA *rsa = ImportKey(pri_key, pri_key_bytes, true);
		if (!rsa)
		{
			return false;
		}

		bool success = false;
		uint32 rsa_size = (uint32)RSA_size(rsa);
		if (sig_bytes >= rsa_size)
		{
			Byte digest[SHA256_DIGEST_LENGTH];
			HashSha256(data, data_size, digest);

			std::vector<Byte> signature(rsa_size);
			unsigned int signature_length = 0;
			if (RSA_sign(NID_sha256, digest, sizeof(digest), signature.data(), &signature_length, rsa) == 1 && signature_length == rsa_size)
			{
				// CryptoAPI expects the signature in little endian order.
				std::reverse(signature.begin(), signature.end());
				memcpy(sig, signature.data(), rsa_size);
				success = true;
			}
		}

		RSA_free(rsa);
		return success;
	}

	bool LinuxCrypto::TestSignature(void const *sig, uint32 sig_bytes, void const *src, uint32 src_bytes, Byte const *pub_key, uint32 pub_key_bytes)
	{
		if (!sig || !src)
		{
			return false;
		}

		RSA *rsa = ImportKey(pub_key, pub_key_bytes, false);
		if (!rsa)
		{
			return false;
		}

		bool success = false;
		uint32 rsa_size = (uint32)RSA_size(rsa);
		if (sig_bytes == rsa_size)
		{
			Byte digest[SHA256_DIGEST_LENGTH];
			HashSha256(src, src_bytes, digest);

			std::vector<Byte> signature((Byte const *)sig, (Byte const *)sig + sig_bytes);
			std::reverse(signature.begin(), signature.end());
			success = RSA_verify(NID_sha256, digest, sizeof(digest), signature.data(), rsa_size, rsa) == 1;
		}

		RSA_free(rsa);
		return success;
	}
}

#endif // CAM_PLATFORM_LINUX
