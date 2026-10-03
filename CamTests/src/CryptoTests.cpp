#include "CamTest.h"
#include "TestUtils.h"

#include <cstring>
#include <memory>
#include <set>

// The size of the signature, which the update server sends (SIG_BYTES in the Message.h of the updater: an RSA key with 4096 bits).
static constexpr uint32 SIGNATURE_BYTES = 512;

struct KeyPair
{
	Core::Crypto::key_t Public;
	Core::Crypto::key_t Private;
};

// Generating a key takes a moment, so all tests share one.
static KeyPair &SharedKeys()
{
	static KeyPair keys;
	static bool generated = false;
	if (!generated)
	{
		std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());
		generated = crypto->GenKeys(&keys.Public, &keys.Private);
	}

	return keys;
}

TEST(Crypto, GeneratesKeysOfTheRightSize)
{
	KeyPair &keys = SharedKeys();
	CHECK(keys.Public.Size > 0 && keys.Public.Size < sizeof(keys.Public.Data));
	CHECK(keys.Private.Size > 0 && keys.Private.Size < sizeof(keys.Private.Data));
	CHECK(keys.Public.Size != keys.Private.Size);
}

TEST(Crypto, SignedDataVerifies)
{
	KeyPair &keys = SharedKeys();
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());

	std::vector<Byte> data(100000, 0x5A);
	Byte signature[SIGNATURE_BYTES] = {};
	REQUIRE(crypto->SignSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), keys.Private.Data, keys.Private.Size));
	CHECK(crypto->TestSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), keys.Public.Data, keys.Public.Size));
}

TEST(Crypto, ChangedDataDoesNotVerify)
{
	KeyPair &keys = SharedKeys();
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());

	std::vector<Byte> data(4096, 0x11);
	Byte signature[SIGNATURE_BYTES] = {};
	REQUIRE(crypto->SignSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), keys.Private.Data, keys.Private.Size));

	data[1000] ^= 1;
	CHECK(!crypto->TestSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), keys.Public.Data, keys.Public.Size));
}

TEST(Crypto, ChangedSignatureDoesNotVerify)
{
	KeyPair &keys = SharedKeys();
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());

	std::vector<Byte> data(4096, 0x22);
	Byte signature[SIGNATURE_BYTES] = {};
	REQUIRE(crypto->SignSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), keys.Private.Data, keys.Private.Size));

	signature[10] ^= 0x80;
	CHECK(!crypto->TestSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), keys.Public.Data, keys.Public.Size));
}

TEST(Crypto, SignatureOfAnotherKeyDoesNotVerify)
{
	KeyPair &keys = SharedKeys();
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());

	// This is the pinned key situation of the updater: a server with another key must not be accepted.
	Core::Crypto::key_t other_public, other_private;
	REQUIRE(crypto->GenKeys(&other_public, &other_private));

	std::vector<Byte> data(4096, 0x33);
	Byte signature[SIGNATURE_BYTES] = {};
	REQUIRE(crypto->SignSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), other_private.Data, other_private.Size));

	CHECK(!crypto->TestSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), keys.Public.Data, keys.Public.Size));
	CHECK(crypto->TestSignature(signature, SIGNATURE_BYTES, data.data(), (uint32)data.size(), other_public.Data, other_public.Size));
}

TEST(Crypto, NormalizeKeyCutsTheGarbageBehindTheKey)
{
	KeyPair &keys = SharedKeys();

	// Older versions stored keys padded with uninitialized memory. Normalizing returns the real key.
	Core::Crypto::key_t padded = keys.Public;
	padded.Size = (uint32)sizeof(padded.Data);
	memset(padded.Data + keys.Public.Size, 0xCD, sizeof(padded.Data) - keys.Public.Size);

	Core::Crypto::NormalizeKey(&padded);
	CHECK_EQ(padded.Size, keys.Public.Size);
	CHECK(memcmp(padded.Data, keys.Public.Data, keys.Public.Size) == 0);

	// A key, which is exact already, stays as it is.
	Core::Crypto::key_t exact = keys.Public;
	Core::Crypto::NormalizeKey(&exact);
	CHECK_EQ(exact.Size, keys.Public.Size);
}

TEST(Crypto, TokensAreNotRepeating)
{
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());
	std::set<uint64> tokens;
	for (int i = 0; i < 200; ++i)
	{
		tokens.insert(crypto->GenToken());
	}

	// 200 random 64 bit numbers are different.
	CHECK_EQ(tokens.size(), (size_t)200);
}
