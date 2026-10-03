#pragma once

// A very small test framework (no dependencies, so the tests build everywhere the project builds).
//
//   TEST(Config, ReadsFile) { CHECK(a == b); CHECK_EQ(a, b); REQUIRE(ptr != nullptr); }
//
// CHECK reports a failure and goes on, REQUIRE reports it and ends the test. The program prints every failure, a summary, and returns a value != 0 if a
// test failed (this is what the GitHub action looks at).
//   CamTests                 runs everything
//   CamTests Config          runs the tests, whose name contains "Config" (several filters can be given)
//   CamTests --list          lists the tests

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace CamTest
{
	struct TestCase
	{
		std::string Name;
		std::function<void()> Body;
	};

	std::vector<TestCase> &Registry();
	void ReportFailure(const char *file, int line, const std::string &message);

	struct Registrar
	{
		Registrar(const std::string &suite, const std::string &name, std::function<void()> body)
		{
			Registry().push_back({ suite + "." + name, std::move(body) });
		}
	};

	// Thrown by REQUIRE to end the test.
	struct RequireFailed {};

	template <typename T>
	std::string Show(const T &value)
	{
		std::ostringstream stream;
		stream << value;
		return stream.str();
	}

	inline std::string Show(const std::string &value) { return "\"" + value + "\""; }
	inline std::string Show(const char *value) { return std::string("\"") + value + "\""; }
	inline std::string Show(bool value) { return value ? "true" : "false"; }
	inline std::string Show(unsigned char value) { return std::to_string((int)value); }

	template <typename A, typename B>
	bool CheckEqual(const A &a, const B &b, const char *file, int line, const char *expression_a, const char *expression_b)
	{
		if (a == b)
		{
			return true;
		}

		ReportFailure(file, line, std::string(expression_a) + " == " + expression_b + "\n    left:  " + Show(a) + "\n    right: " + Show(b));
		return false;
	}

	int Run(int argc, char *argv[]);
}

#define CAMTEST_CONCAT2(a, b) a##b
#define CAMTEST_CONCAT(a, b) CAMTEST_CONCAT2(a, b)

#define TEST(suite, name) \
	static void CAMTEST_CONCAT(CamTest_##suite##_, name)(); \
	static CamTest::Registrar CAMTEST_CONCAT(CamTestRegistrar_##suite##_, name)(#suite, #name, CAMTEST_CONCAT(CamTest_##suite##_, name)); \
	static void CAMTEST_CONCAT(CamTest_##suite##_, name)()

#define CHECK(condition) \
	do { if (!(condition)) CamTest::ReportFailure(__FILE__, __LINE__, "CHECK(" #condition ")"); } while (0)

#define CHECK_EQ(a, b) \
	do { CamTest::CheckEqual((a), (b), __FILE__, __LINE__, #a, #b); } while (0)

#define REQUIRE(condition) \
	do { if (!(condition)) { CamTest::ReportFailure(__FILE__, __LINE__, "REQUIRE(" #condition ")"); throw CamTest::RequireFailed(); } } while (0)

#define REQUIRE_EQ(a, b) \
	do { if (!CamTest::CheckEqual((a), (b), __FILE__, __LINE__, #a, #b)) throw CamTest::RequireFailed(); } while (0)
