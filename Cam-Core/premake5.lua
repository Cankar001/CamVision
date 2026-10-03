project "Cam-Core"
    kind "StaticLib"
    language "C++"
	cppdialect "C++17"
	staticruntime "off"


    files
    { 
        "src/**.h",
        "src/**.cpp",
    }

    includedirs
    {
		"src",
		"%{IncludeDir.miniz}",
		"%{IncludeDir.spdlog}",
    }

	links
	{
		"Miniz",
		"spdlog"
	}

    defines
	{
		"_CRT_SECURE_NO_WARNINGS",
		"CAM_LIBRARY_EXPORT"
	}

    filter "system:windows"
        systemversion "latest"

        defines
        {
            "CAM_PLATFORM_WINDOWS",
        }

		links
		{
			"Ws2_32.lib"
		}

	filter "system:macosx"
        systemversion "latest"
		
        defines
        {
            "CAM_PLATFORM_MAC",
        }

	filter "system:linux"
		systemversion "latest"

		defines
        {
            "CAM_PLATFORM_LINUX",
        }

    filter "configurations:Debug"
        symbols "On"

		defines
		{
			"CAM_DEBUG",
		}

    filter "configurations:Release"
        optimize "On"
		
		defines
		{
			"CAM_RELEASE",
			"NDEBUG",
		}

