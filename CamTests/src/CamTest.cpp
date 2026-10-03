#include "CamTest.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>

namespace CamTest
{
	static int s_FailuresInTest = 0;

	std::vector<TestCase> &Registry()
	{
		static std::vector<TestCase> registry;
		return registry;
	}

	void ReportFailure(const char *file, int line, const std::string &message)
	{
		++s_FailuresInTest;
		std::cout << "  FAILED " << file << ":" << line << "\n    " << message << std::endl;
	}

	int Run(int argc, char *argv[])
	{
		std::vector<std::string> filters;
		bool list_only = false;
		for (int i = 1; i < argc; ++i)
		{
			if (strcmp(argv[i], "--list") == 0)
			{
				list_only = true;
			}
			else if (argv[i][0] != '-')
			{
				filters.push_back(argv[i]);
			}
		}

		std::vector<const TestCase *> selected;
		for (const TestCase &test : Registry())
		{
			bool match = filters.empty();
			for (const std::string &filter : filters)
			{
				match = match || test.Name.find(filter) != std::string::npos;
			}

			if (match)
			{
				selected.push_back(&test);
			}
		}

		if (list_only)
		{
			for (const TestCase *test : selected)
			{
				std::cout << test->Name << "\n";
			}

			return 0;
		}

		std::vector<std::string> failed;
		for (const TestCase *test : selected)
		{
			std::cout << "[ RUN  ] " << test->Name << std::endl;
			s_FailuresInTest = 0;

			auto start = std::chrono::steady_clock::now();
			try
			{
				test->Body();
			}
			catch (const RequireFailed &)
			{
			}
			catch (const std::exception &exception)
			{
				ReportFailure("(exception)", 0, std::string("unexpected exception: ") + exception.what());
			}
			catch (...)
			{
				ReportFailure("(exception)", 0, "unexpected exception");
			}

			long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			if (s_FailuresInTest == 0)
			{
				std::cout << "[  OK  ] " << test->Name << " (" << ms << " ms)" << std::endl;
			}
			else
			{
				std::cout << "[ FAIL ] " << test->Name << " (" << ms << " ms)" << std::endl;
				failed.push_back(test->Name);
			}
		}

		std::cout << "\n" << (selected.size() - failed.size()) << " of " << selected.size() << " tests passed." << std::endl;
		for (const std::string &name : failed)
		{
			std::cout << "  failed: " << name << std::endl;
		}

		return failed.empty() ? 0 : 1;
	}
}
