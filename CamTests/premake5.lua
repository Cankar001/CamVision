project "CamTests"
    kind "ConsoleApp"
    language "C++"
	cppdialect "C++17"
	staticruntime "off"
	entrypoint "mainCRTStartup"


	dependson
	{
		"Cam-Core",
		"Miniz"
	}

    files
    {
        "src/**.h",
        "src/**.cpp",

		-- Parts of the server, which do not need OpenCV, are compiled into the tests directly (the server is a program, not a library).
		"../CamServer/src/Recorder.h",
		"../CamServer/src/Recorder.cpp"
    }

    includedirs
    {
		"src",
		"../CamServer/src",
		"%{IncludeDir.cam_core}",
		"%{IncludeDir.spdlog}",
    }

	links
	{
		"Cam-Core",
		"Miniz",
		"spdlog",
	}

    filter "system:windows"
        systemversion "latest"
		defines "CAM_PLATFORM_WINDOWS"

    filter "system:linux"
        systemversion "latest"
		defines "CAM_PLATFORM_LINUX"

        links
        {
            "pthread",
			"anl",
			"ssl",
			"crypto",
			"atomic",
        }

    filter "configurations:Debug"
        defines "CAM_DEBUG"
        symbols "On"

    filter "configurations:Release"
        defines "CAM_RELEASE"
        optimize "On"
