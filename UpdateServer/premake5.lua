project "UpdateServer"
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
        "src/**.cpp"
    }

    includedirs
    {
		"src",
		"%{IncludeDir.cam_core}",
		"%{IncludeDir.spdlog}",
    }
	
	postbuildcommands
	{
	}
	
	links
	{
		"Cam-Core",
		"Miniz",
		"spdlog",
	}

    filter "system:windows"
        systemversion "latest"

    filter "system:linux"
        systemversion "latest"
		defines "CAM_PLATFORM_LINUX"

        links
        {
            "pthread",
			"anl",
			"ssl",
			"crypto",
        }

    filter "configurations:Debug"
        defines { "CAM_DEBUG", "NDEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "CAM_RELEASE", "NDEBUG" }
        optimize "On"
		
		