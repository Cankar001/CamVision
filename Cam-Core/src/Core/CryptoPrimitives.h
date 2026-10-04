#pragma once

#include "Core.h"

namespace Core::crypto
{
	// The building blocks of the secure connection (see Net/SecureSocket.h). They come from the system (Windows CNG, OpenSSL on Linux), nothing is
	// implemented by hand.

	constexpr uint32 KEY_BYTES = 32;		// AES-256, and the size of all keys of this project
	constexpr uint32 NONCE_BYTES = 12;
	constexpr uint32 TAG_BYTES = 16;
	constexpr uint32 SHA256_BYTES = 32;

	/// <summary>
	/// Random bytes from the random number generator of the system (secure enough for keys).
	/// </summary>
	bool RandomBytes(void *dst, uint32 bytes);

	void Sha256(const void *data, uint32 bytes, Byte out[SHA256_BYTES]);

	void HmacSha256(const void *key, uint32 keyBytes, const void *data, uint32 bytes, Byte out[SHA256_BYTES]);

	/// <summary>
	/// HKDF with SHA-256 (RFC 5869): makes any number of keys (up to 255 * 32 bytes) out of one secret.
	/// </summary>
	void HkdfSha256(const void *salt, uint32 saltBytes, const void *secret, uint32 secretBytes, const void *info, uint32 infoBytes, Byte *out, uint32 outBytes);

	/// <summary>
	/// AES-256-GCM: encrypts the data and creates the tag, which protects the data and the additional data (aad, not encrypted). The nonce must never be used twice with
	/// the same key. ciphertext has the size of the plain text and may be the same buffer.
	/// </summary>
	bool AesGcmSeal(const Byte key[KEY_BYTES], const Byte nonce[NONCE_BYTES], const void *aad, uint32 aadBytes, const void *plain, uint32 bytes, void *ciphertext, Byte tag[TAG_BYTES]);

	/// <summary>
	/// Decrypts the data, if the tag matches the data, the additional data, the key and the nonce. Returns false (and no data) for everything, which was
	/// not made with exactly these values.
	/// </summary>
	bool AesGcmOpen(const Byte key[KEY_BYTES], const Byte nonce[NONCE_BYTES], const void *aad, uint32 aadBytes, const void *ciphertext, uint32 bytes, const Byte tag[TAG_BYTES], void *plain);

	/// <summary>
	/// Compares without stopping at the first difference (the time does not tell, how many bytes were right).
	/// </summary>
	bool ConstantTimeEquals(const void *a, const void *b, uint32 bytes);
}
