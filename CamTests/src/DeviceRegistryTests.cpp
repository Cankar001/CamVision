#include "CamTest.h"
#include "TestUtils.h"

#include "Net/DeviceRegistry.h"

#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

namespace
{
	std::string ReadFile(const std::string &path)
	{
		std::ifstream stream(path, std::ios::binary);
		std::stringstream content;
		content << stream.rdbuf();
		return content.str();
	}

	const char *KEY_A = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
	const char *KEY_B = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
}

TEST(DeviceRegistry, HexConversion)
{
	Byte bytes[4] = { 0x00, 0x7f, 0x80, 0xff };
	CHECK_EQ(Core::BytesToHex(bytes, 4), "007f80ff");

	Byte parsed[4] = {};
	CHECK(Core::HexToBytes("007F80ff", parsed, 4));
	CHECK(memcmp(parsed, bytes, 4) == 0);
	CHECK(Core::HexToBytes("  007f80ff \n", parsed, 4));

	// Only exactly the right text is accepted.
	CHECK(!Core::HexToBytes("007f80", parsed, 4));
	CHECK(!Core::HexToBytes("007f80ff00", parsed, 4));
	CHECK(!Core::HexToBytes("007f80fg", parsed, 4));
	CHECK(!Core::HexToBytes("", parsed, 4));
}

TEST(DeviceRegistry, AddMakesAKeyAndStoresIt)
{
	TempDir dir;
	Core::DeviceRegistry registry(dir.File("devices.cfg"));

	Core::Device device;
	std::string error;
	REQUIRE(registry.Add(Core::DeviceRole::Camera, "Front door", &device, &error));
	CHECK_EQ(device.Name, "Front door");
	CHECK(device.Role == Core::DeviceRole::Camera);

	// The key is found by its id, which does not tell the key.
	Core::Device found;
	REQUIRE(registry.Find(device.KeyId, &found));
	CHECK_EQ(found.Name, "Front door");
	CHECK(memcmp(found.Key, device.Key, 32) == 0);
	CHECK(device.KeyId == Core::DeviceKeyId(device.Key));

	// A new registry (like a restarted server) reads the same from the file.
	Core::DeviceRegistry reread(dir.File("devices.cfg"));
	std::string load_error;
	REQUIRE(reread.Load(&load_error));
	REQUIRE_EQ(reread.Count(), 1u);
	REQUIRE(reread.Find(device.KeyId, &found));
	CHECK(memcmp(found.Key, device.Key, 32) == 0);
	CHECK(found.Role == Core::DeviceRole::Camera);
}

TEST(DeviceRegistry, KeysAreDifferent)
{
	TempDir dir;
	Core::DeviceRegistry registry(dir.File("devices.cfg"));

	std::set<std::string> keys;
	std::set<uint64> ids;
	for (int i = 0; i < 20; ++i)
	{
		Core::Device device;
		std::string error;
		REQUIRE(registry.Add(i % 2 ? Core::DeviceRole::Display : Core::DeviceRole::Camera, "device " + std::to_string(i), &device, &error));
		keys.insert(Core::BytesToHex(device.Key, 32));
		ids.insert(device.KeyId);
	}

	CHECK_EQ(keys.size(), (size_t)20);
	CHECK_EQ(ids.size(), (size_t)20);
}

TEST(DeviceRegistry, FileHasTheDocumentedFormat)
{
	TempDir dir;
	Core::DeviceRegistry registry(dir.File("devices.cfg"));

	Core::Device camera, display;
	std::string error;
	REQUIRE(registry.Add(Core::DeviceRole::Camera, "Front door", &camera, &error));
	REQUIRE(registry.Add(Core::DeviceRole::Display, "Living room", &display, &error));

	std::string content = ReadFile(dir.File("devices.cfg"));
	CHECK(content.find("camera " + Core::BytesToHex(camera.Key, 32) + " Front door\n") != std::string::npos);
	CHECK(content.find("display " + Core::BytesToHex(display.Key, 32) + " Living room\n") != std::string::npos);
	CHECK(content.find('#') == 0);		// starts with the comment, which explains the file
}

TEST(DeviceRegistry, LoadsAHandWrittenFile)
{
	TempDir dir;
	std::ofstream(dir.File("devices.cfg")) << "# comment\n\ncamera " << KEY_A << " Garden camera\n   display   " << KEY_B << "   Kitchen display   \n";

	Core::DeviceRegistry registry(dir.File("devices.cfg"));
	std::string error;
	REQUIRE(registry.Load(&error));

	std::vector<Core::Device> devices = registry.List();
	REQUIRE_EQ(devices.size(), (size_t)2);
	CHECK_EQ(devices[0].Name, "Garden camera");
	CHECK(devices[0].Role == Core::DeviceRole::Camera);
	CHECK_EQ(Core::BytesToHex(devices[0].Key, 32), KEY_A);
	CHECK_EQ(devices[1].Name, "Kitchen display");		// the spaces around are not part of the name
	CHECK(devices[1].Role == Core::DeviceRole::Display);
}

TEST(DeviceRegistry, BrokenLinesAreReportedAndTheRestIsUsed)
{
	TempDir dir;
	std::ofstream(dir.File("devices.cfg")) << "camera " << KEY_A << " Good\ncamera 1234 Short key\nrobot " << KEY_B << " Wrong role\ncamera " << KEY_B << "\n";

	Core::DeviceRegistry registry(dir.File("devices.cfg"));
	std::string error;
	CHECK(!registry.Load(&error));
	CHECK(error.find("line 2") != std::string::npos);
	CHECK(error.find("line 3") != std::string::npos);
	CHECK(error.find("line 4") != std::string::npos);		// without a name
	CHECK_EQ(registry.Count(), 1u);
}

TEST(DeviceRegistry, AMissingFileIsAnEmptyList)
{
	TempDir dir;
	Core::DeviceRegistry registry(dir.File("missing.cfg"));
	std::string error;
	CHECK(registry.Load(&error));
	CHECK_EQ(registry.Count(), 0u);

	Core::Device found;
	CHECK(!registry.Find(12345, &found));
}

TEST(DeviceRegistry, NamesAreChecked)
{
	TempDir dir;
	Core::DeviceRegistry registry(dir.File("devices.cfg"));
	std::string error;

	REQUIRE(registry.Add(Core::DeviceRole::Camera, "Front door", nullptr, &error));

	// the same name (also in another case, and for the other role)
	CHECK(!registry.Add(Core::DeviceRole::Camera, "Front door", nullptr, &error));
	CHECK(!registry.Add(Core::DeviceRole::Display, "FRONT DOOR", nullptr, &error));
	CHECK(error.find("already") != std::string::npos);

	CHECK(!registry.Add(Core::DeviceRole::Camera, "", nullptr, &error));
	CHECK(!registry.Add(Core::DeviceRole::Camera, std::string(32, 'x'), nullptr, &error));
	CHECK(registry.Add(Core::DeviceRole::Camera, std::string(31, 'x'), nullptr, &error));
	CHECK(!registry.Add(Core::DeviceRole::Camera, "bad\nname", nullptr, &error));
	CHECK(!registry.Add(Core::DeviceRole::None, "No role", nullptr, &error));

	CHECK_EQ(registry.Count(), 2u);
}

TEST(DeviceRegistry, RemoveTakesTheKeyAway)
{
	TempDir dir;
	Core::DeviceRegistry registry(dir.File("devices.cfg"));
	Core::Device first, second;
	std::string error;
	REQUIRE(registry.Add(Core::DeviceRole::Camera, "First", &first, &error));
	REQUIRE(registry.Add(Core::DeviceRole::Camera, "Second", &second, &error));

	REQUIRE(registry.Remove("first", &error));		// the case does not matter
	Core::Device found;
	CHECK(!registry.Find(first.KeyId, &found));
	CHECK(registry.Find(second.KeyId, &found));
	CHECK(!registry.Remove("First", &error));

	// Also in the file.
	Core::DeviceRegistry reread(dir.File("devices.cfg"));
	REQUIRE(reread.Load(&error));
	CHECK_EQ(reread.Count(), 1u);
	CHECK(!reread.Find(first.KeyId, &found));
}

TEST(DeviceRegistry, ChangesOfTheFileAreNoticed)
{
	TempDir dir;
	Core::DeviceRegistry server(dir.File("devices.cfg"));
	std::string error;
	REQUIRE(server.Load(&error));
	CHECK(!server.ReloadIfChanged());

	// Somebody else (CamServer --add_device in another console) adds a device.
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	Core::DeviceRegistry tool(dir.File("devices.cfg"));
	Core::Device added;
	REQUIRE(tool.Add(Core::DeviceRole::Display, "New display", &added, &error));

	CHECK(server.ReloadIfChanged());
	Core::Device found;
	CHECK(server.Find(added.KeyId, &found));
	CHECK(!server.ReloadIfChanged());		// nothing changed since
}

TEST(DeviceRegistry, RoleNames)
{
	CHECK(Core::DeviceRoleFromName("camera") == Core::DeviceRole::Camera);
	CHECK(Core::DeviceRoleFromName(" Display ") == Core::DeviceRole::Display);
	CHECK(Core::DeviceRoleFromName("admin") == Core::DeviceRole::None);
	CHECK_EQ(std::string(Core::DeviceRoleName(Core::DeviceRole::Camera)), "camera");
}
