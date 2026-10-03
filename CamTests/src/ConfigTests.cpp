#include "CamTest.h"
#include "TestUtils.h"

#include <fstream>

static Core::Config LoadConfig(const std::string &file_content, std::vector<std::string> arguments = {})
{
	static TempDir dir;
	static int counter = 0;
	std::string file = dir.File("test" + std::to_string(++counter) + ".cfg");
	std::ofstream(file) << file_content;

	// Config takes a char *argv[], the strings have to stay alive while it is read.
	std::vector<std::string> storage = { "program", "--config=" + file };
	storage.insert(storage.end(), arguments.begin(), arguments.end());
	std::vector<char *> argv;
	for (std::string &argument : storage)
	{
		argv.push_back(argument.data());
	}

	return Core::Config((int)argv.size(), argv.data(), "");
}

TEST(Config, ReadsKeyValuePairs)
{
	Core::Config config = LoadConfig("server_ip = 10.0.0.5\nserver_port=4000\n");
	CHECK_EQ(config.GetString("server_ip", ""), "10.0.0.5");
	CHECK_EQ(config.GetInt("server_port", 0), 4000);
}

TEST(Config, IgnoresCommentsAndEmptyLines)
{
	Core::Config config = LoadConfig("# a comment\n\n   # indented comment\nname = Front # not a comment\nbroken line without separator\n");
	CHECK_EQ(config.GetString("name", ""), "Front # not a comment");
}

TEST(Config, KeysAreCaseInsensitiveAndDashEqualsUnderscore)
{
	Core::Config config = LoadConfig("Server-IP = 1.2.3.4\n");
	CHECK_EQ(config.GetString("server_ip", ""), "1.2.3.4");
	CHECK_EQ(config.GetString("SERVER-IP", ""), "1.2.3.4");
}

TEST(Config, FallbackForMissingKeys)
{
	Core::Config config = LoadConfig("");
	CHECK_EQ(config.GetString("x", "fallback"), "fallback");
	CHECK_EQ(config.GetInt("x", 7), 7);
	CHECK_EQ(config.GetBool("x", true), true);
	CHECK(config.GetFloat("x", 1.5f) == 1.5f);
}

TEST(Config, CommandLineOverridesFile)
{
	Core::Config config = LoadConfig("port = 1\nname = file\n", { "--port=2", "--headless", "ignored_without_dashes" });
	CHECK_EQ(config.GetInt("port", 0), 2);
	CHECK_EQ(config.GetString("name", ""), "file");
	CHECK_EQ(config.GetBool("headless", false), true);
}

TEST(Config, BooleanSpellings)
{
	Core::Config config = LoadConfig("a = yes\nb = 0\nc = ON\nd = False\ne = maybe\n");
	CHECK_EQ(config.GetBool("a", false), true);
	CHECK_EQ(config.GetBool("b", true), false);
	CHECK_EQ(config.GetBool("c", false), true);
	CHECK_EQ(config.GetBool("d", true), false);
	CHECK_EQ(config.GetBool("e", true), true);		// invalid: the fallback
	CHECK_EQ(config.GetBool("e", false), false);
}

TEST(Config, InvalidNumbersUseTheFallback)
{
	Core::Config config = LoadConfig("a = abc\nb = 12x\nc = 3.25\nd = -5\n");
	CHECK_EQ(config.GetInt("a", 9), 9);
	CHECK_EQ(config.GetInt("b", 9), 9);
	CHECK_EQ(config.GetInt("d", 9), -5);
	CHECK(config.GetFloat("c", 0.0f) == 3.25f);
	CHECK(config.GetFloat("a", 2.0f) == 2.0f);
}

TEST(Config, ReportsTheLoadedFile)
{
	Core::Config config = LoadConfig("a = 1\n");
	CHECK(!config.LoadedFile().empty());

	char program[] = "program";
	char *argv[] = { program };
	Core::Config missing(1, argv, "this_file_does_not_exist.cfg");
	CHECK(missing.LoadedFile().empty());
}
