#include "CamTest.h"
#include "TestUtils.h"

#include <cstring>
#include <memory>

// The signature of an update covers its version. These tests play the attack, which this prevents: somebody, who can send packets in the name of the
// update server, offers an old update (with its valid signature) under a higher version number.

static constexpr uint32 SIGNATURE_BYTES = 512;

struct Keys
{
	Core::Crypto::key_t Public;
	Core::Crypto::key_t Private;
};

static Keys &SharedKeys()
{
	static Keys keys;
	static bool generated = false;
	if (!generated)
	{
		std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());
		generated = crypto->GenKeys(&keys.Public, &keys.Private);
	}

	return keys;
}

// What the update server does.
static void SignUpdate(uint32 version, const std::vector<Byte> &update, Byte *signature)
{
	Keys &keys = SharedKeys();
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());
	std::vector<Byte> signed_data = Core::utils::BuildSignedUpdateData(version, update.data(), (uint32)update.size());
	REQUIRE(crypto->SignSignature(signature, SIGNATURE_BYTES, signed_data.data(), (uint32)signed_data.size(), keys.Private.Data, keys.Private.Size));
}

// What the update client does, with the version the server announced.
static bool AcceptUpdate(uint32 announced_version, const std::vector<Byte> &update, const Byte *signature)
{
	Keys &keys = SharedKeys();
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());
	std::vector<Byte> signed_data = Core::utils::BuildSignedUpdateData(announced_version, update.data(), (uint32)update.size());
	return crypto->TestSignature(signature, SIGNATURE_BYTES, signed_data.data(), (uint32)signed_data.size(), keys.Public.Data, keys.Public.Size);
}

TEST(UpdateSignature, TheRightVersionIsAccepted)
{
	std::vector<Byte> update(50000, 0x42);
	Byte signature[SIGNATURE_BYTES] = {};
	SignUpdate(101, update, signature);
	CHECK(AcceptUpdate(101, update, signature));
}

TEST(UpdateSignature, AnOldUpdateUnderAHigherVersionIsRejected)
{
	std::vector<Byte> old_update(50000, 0x01);
	Byte old_signature[SIGNATURE_BYTES] = {};
	SignUpdate(100, old_update, old_signature);

	// The attacker has the old update with its genuine signature, and announces version 200.
	CHECK(!AcceptUpdate(200, old_update, old_signature));
	CHECK(!AcceptUpdate(101, old_update, old_signature));
	CHECK(AcceptUpdate(100, old_update, old_signature));
}

TEST(UpdateSignature, EveryVersionHasItsOwnSignature)
{
	std::vector<Byte> update(1000, 0x07);
	Byte first[SIGNATURE_BYTES] = {}, second[SIGNATURE_BYTES] = {};
	SignUpdate(5, update, first);
	SignUpdate(6, update, second);

	// The same file, signed for two versions: each signature is only good for its version.
	CHECK(memcmp(first, second, SIGNATURE_BYTES) != 0);
	CHECK(AcceptUpdate(5, update, first));
	CHECK(!AcceptUpdate(6, update, first));
	CHECK(AcceptUpdate(6, update, second));
	CHECK(!AcceptUpdate(5, update, second));
}

TEST(UpdateSignature, ChangedFileIsRejected)
{
	std::vector<Byte> update(5000, 0x33);
	Byte signature[SIGNATURE_BYTES] = {};
	SignUpdate(9, update, signature);

	update[2500] ^= 1;
	CHECK(!AcceptUpdate(9, update, signature));

	// A shorter or longer file with the same beginning is rejected too.
	update[2500] ^= 1;
	update.push_back(0);
	CHECK(!AcceptUpdate(9, update, signature));
	update.pop_back();
	update.pop_back();
	CHECK(!AcceptUpdate(9, update, signature));
}

TEST(UpdateSignature, ASignatureOfTheFileAloneIsNotAnUpdateSignature)
{
	// What the servers of older versions signed: just the file. It must not be accepted for any version anymore.
	Keys &keys = SharedKeys();
	std::unique_ptr<Core::Crypto> crypto(Core::Crypto::Create());

	std::vector<Byte> update(5000, 0x44);
	Byte signature[SIGNATURE_BYTES] = {};
	REQUIRE(crypto->SignSignature(signature, SIGNATURE_BYTES, update.data(), (uint32)update.size(), keys.Private.Data, keys.Private.Size));

	for (uint32 version : { 0u, 1u, 100u, 101u })
	{
		CHECK(!AcceptUpdate(version, update, signature));
	}
}

TEST(UpdateSignature, TheSignedDataHasAFixedLayout)
{
	// Servers and clients of different machines must build exactly the same bytes: magic, version and size (little endian), then the file.
	const Byte file[3] = { 0xAA, 0xBB, 0xCC };
	std::vector<Byte> data = Core::utils::BuildSignedUpdateData(0x01020304u, file, 3);

	const Byte expected[] = { 'C', 'A', 'M', 'U', 'P', 'D', '0', '1', 0x04, 0x03, 0x02, 0x01, 0x03, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC };
	REQUIRE_EQ(data.size(), sizeof(expected));
	CHECK(memcmp(data.data(), expected, sizeof(expected)) == 0);
}
