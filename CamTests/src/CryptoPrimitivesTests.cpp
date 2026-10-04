#include "CamTest.h"
#include "TestUtils.h"

#include "Core/CryptoPrimitives.h"
#include "Net/DeviceRegistry.h"

#include <cstring>
#include <string>
#include <vector>

// The building blocks of the secure connection, checked against the published test vectors of the algorithms. They run on every platform, so the Windows
// (CNG) and the Linux (OpenSSL) implementation are proven to calculate the same.

namespace
{
	std::string Hex(const Byte *data, uint32 bytes)
	{
		return Core::BytesToHex(data, bytes);
	}

	std::vector<Byte> FromHex(const std::string &text)
	{
		std::vector<Byte> bytes(text.size() / 2);
		REQUIRE(Core::HexToBytes(text, bytes.data(), (uint32)bytes.size()));
		return bytes;
	}
}

TEST(CryptoPrimitives, Sha256OfAbc)
{
	Byte hash[32];
	Core::crypto::Sha256("abc", 3, hash);
	CHECK_EQ(Hex(hash, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(CryptoPrimitives, Sha1OfAbcAndNothing)
{
	// Only used for the handshake of the WebSocket protocol.
	Byte hash[20];
	Core::crypto::Sha1("abc", 3, hash);
	CHECK_EQ(Hex(hash, 20), "a9993e364706816aba3e25717850c26c9cd0d89d");
	Core::crypto::Sha1("", 0, hash);
	CHECK_EQ(Hex(hash, 20), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(CryptoPrimitives, Sha256OfNothing)
{
	Byte hash[32];
	Core::crypto::Sha256("", 0, hash);
	CHECK_EQ(Hex(hash, 32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(CryptoPrimitives, HmacSha256Rfc4231Case1)
{
	std::vector<Byte> key(20, 0x0b);
	Byte mac[32];
	Core::crypto::HmacSha256(key.data(), (uint32)key.size(), "Hi There", 8, mac);
	CHECK_EQ(Hex(mac, 32), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(CryptoPrimitives, HmacSha256Rfc4231Case2)
{
	Byte mac[32];
	Core::crypto::HmacSha256("Jefe", 4, "what do ya want for nothing?", 28, mac);
	CHECK_EQ(Hex(mac, 32), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(CryptoPrimitives, HkdfSha256Rfc5869Case1)
{
	std::vector<Byte> secret(22, 0x0b);
	std::vector<Byte> salt = FromHex("000102030405060708090a0b0c");
	std::vector<Byte> info = FromHex("f0f1f2f3f4f5f6f7f8f9");

	Byte out[42];
	Core::crypto::HkdfSha256(salt.data(), (uint32)salt.size(), secret.data(), (uint32)secret.size(), info.data(), (uint32)info.size(), out, sizeof(out));
	CHECK_EQ(Hex(out, 42), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
}

TEST(CryptoPrimitives, HkdfSha256Rfc5869Case3WithoutSaltAndInfo)
{
	std::vector<Byte> secret(22, 0x0b);

	Byte out[42];
	Core::crypto::HkdfSha256(nullptr, 0, secret.data(), (uint32)secret.size(), nullptr, 0, out, sizeof(out));
	CHECK_EQ(Hex(out, 42), "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
}

TEST(CryptoPrimitives, AesGcmTestCase13EmptyData)
{
	// McGrew and Viega, test case 13: key of zeros, nonce of zeros, nothing to encrypt.
	Byte key[32] = {}, nonce[12] = {}, tag[16] = {};
	CHECK(Core::crypto::AesGcmSeal(key, nonce, nullptr, 0, nullptr, 0, nullptr, tag));
	CHECK_EQ(Hex(tag, 16), "530f8afbc74536b9a963b4f1c4cb738b");
}

TEST(CryptoPrimitives, AesGcmTestCase14)
{
	// Test case 14: one block of zeros.
	Byte key[32] = {}, nonce[12] = {}, plain[16] = {}, cipher[16], tag[16];
	CHECK(Core::crypto::AesGcmSeal(key, nonce, nullptr, 0, plain, 16, cipher, tag));
	CHECK_EQ(Hex(cipher, 16), "cea7403d4d606b6e074ec5d3baf39d18");
	CHECK_EQ(Hex(tag, 16), "d0d1c8a799996bf0265b98b5d48ab919");

	Byte opened[16];
	CHECK(Core::crypto::AesGcmOpen(key, nonce, nullptr, 0, cipher, 16, tag, opened));
	CHECK(memcmp(opened, plain, 16) == 0);
}

TEST(CryptoPrimitives, AesGcmTestCase16WithAdditionalData)
{
	// Test case 16: AES-256, a longer text and additional data.
	std::vector<Byte> key = FromHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
	std::vector<Byte> nonce = FromHex("cafebabefacedbaddecaf888");
	std::vector<Byte> plain = FromHex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
	std::vector<Byte> aad = FromHex("feedfacedeadbeeffeedfacedeadbeefabaddad2");

	std::vector<Byte> cipher(plain.size());
	Byte tag[16];
	REQUIRE(Core::crypto::AesGcmSeal(key.data(), nonce.data(), aad.data(), (uint32)aad.size(), plain.data(), (uint32)plain.size(), cipher.data(), tag));
	CHECK_EQ(Hex(cipher.data(), (uint32)cipher.size()), "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
	CHECK_EQ(Hex(tag, 16), "76fc6ece0f4e1768cddf8853bb2d551b");
}

TEST(CryptoPrimitives, AesGcmRefusesEveryChange)
{
	Byte key[32], nonce[12], tag[16];
	REQUIRE(Core::crypto::RandomBytes(key, 32));
	REQUIRE(Core::crypto::RandomBytes(nonce, 12));

	const char message[] = "the front door is open";
	const char aad[] = "header";
	std::vector<Byte> cipher(sizeof(message));
	REQUIRE(Core::crypto::AesGcmSeal(key, nonce, aad, sizeof(aad), message, sizeof(message), cipher.data(), tag));
	CHECK(memcmp(cipher.data(), message, sizeof(message)) != 0);		// it is encrypted

	std::vector<Byte> opened(sizeof(message));
	CHECK(Core::crypto::AesGcmOpen(key, nonce, aad, sizeof(aad), cipher.data(), (uint32)cipher.size(), tag, opened.data()));
	CHECK(memcmp(opened.data(), message, sizeof(message)) == 0);

	// A changed text, tag, additional data, nonce or key: nothing opens.
	std::vector<Byte> changed = cipher;
	changed[3] ^= 1;
	CHECK(!Core::crypto::AesGcmOpen(key, nonce, aad, sizeof(aad), changed.data(), (uint32)changed.size(), tag, opened.data()));

	Byte changed_tag[16];
	memcpy(changed_tag, tag, 16);
	changed_tag[15] ^= 0x80;
	CHECK(!Core::crypto::AesGcmOpen(key, nonce, aad, sizeof(aad), cipher.data(), (uint32)cipher.size(), changed_tag, opened.data()));

	CHECK(!Core::crypto::AesGcmOpen(key, nonce, "Header", sizeof(aad), cipher.data(), (uint32)cipher.size(), tag, opened.data()));

	Byte other_nonce[12];
	memcpy(other_nonce, nonce, 12);
	other_nonce[11] ^= 1;
	CHECK(!Core::crypto::AesGcmOpen(key, other_nonce, aad, sizeof(aad), cipher.data(), (uint32)cipher.size(), tag, opened.data()));

	Byte other_key[32];
	memcpy(other_key, key, 32);
	other_key[0] ^= 1;
	CHECK(!Core::crypto::AesGcmOpen(other_key, nonce, aad, sizeof(aad), cipher.data(), (uint32)cipher.size(), tag, opened.data()));
}

TEST(CryptoPrimitives, AesGcmWorksInPlace)
{
	Byte key[32], nonce[12], tag[16];
	REQUIRE(Core::crypto::RandomBytes(key, 32));
	REQUIRE(Core::crypto::RandomBytes(nonce, 12));

	std::vector<Byte> data(1200, 0x5A);
	std::vector<Byte> original = data;
	REQUIRE(Core::crypto::AesGcmSeal(key, nonce, nullptr, 0, data.data(), (uint32)data.size(), data.data(), tag));
	CHECK(data != original);
	REQUIRE(Core::crypto::AesGcmOpen(key, nonce, nullptr, 0, data.data(), (uint32)data.size(), tag, data.data()));
	CHECK(data == original);
}

TEST(CryptoPrimitives, RandomBytesDiffer)
{
	Byte a[32], b[32];
	REQUIRE(Core::crypto::RandomBytes(a, 32));
	REQUIRE(Core::crypto::RandomBytes(b, 32));
	CHECK(memcmp(a, b, 32) != 0);
}

TEST(CryptoPrimitives, ConstantTimeEquals)
{
	Byte a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 4 }, c[4] = { 1, 2, 3, 5 };
	CHECK(Core::crypto::ConstantTimeEquals(a, b, 4));
	CHECK(!Core::crypto::ConstantTimeEquals(a, c, 4));
	CHECK(Core::crypto::ConstantTimeEquals(a, c, 3));
}
