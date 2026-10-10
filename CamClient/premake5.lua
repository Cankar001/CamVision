project "CamClient"
    kind "ConsoleApp"
    language "C++"
	cppdialect "C++17"
	staticruntime "off"
	entrypoint "mainCRTStartup"
	
	dependson
	{
		"Cam-Core",
		"OpenCV"
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

    links
    {
        "Cam-Core",
		"spdlog"
    }

	filter { "system:windows", "configurations:Debug" }
		includedirs { "%{IncludeDir.opencv}" }
        systemversion "latest"
		symbols "On"

        defines
        {
            "CAM_PLATFORM_WINDOWS",
			"CAM_DEBUG"
        }

		links
		{
			"%{LibDir.opencv_world_debug}",
		}


	filter { "system:linux", "configurations:Debug" }
		systemversion "latest"
		symbols "On"
		
		defines
		{
			"CAM_PLATFORM_LINUX",
			"CAM_DEBUG"
		}

		-- OpenCV comes from the system (e.g. "sudo apt install libopencv-dev"), so it matches the CPU architecture (x86_64, ARM) of the machine.
		buildoptions { "`pkg-config --cflags opencv4`" }
		linkoptions { "`pkg-config --libs opencv4`" }

		links
		{
			"pthread",
			"anl",
			"ssl",
			"crypto",
			"atomic"
		}

	filter { "system:macos", "configurations:Debug" }
		systemversion "latest"
		symbols "On"
		
		defines
		{
			"CAM_PLATFORM_MACOS",
			"CAM_DEBUG"
		}

		links
		{

		}

	filter { "system:windows", "configurations:Release" }
		includedirs { "%{IncludeDir.opencv}" }
		systemversion "latest"
        optimize "On"

		defines
		{
			"CAM_PLATFORM_WINDOWS",
			"CAM_RELEASE"
		}

		links
		{
			"%{LibDir.opencv_world}",
		}
		

	filter { "system:linux", "configurations:Release" }
		systemversion "latest"
        optimize "On"

		defines
		{
			"CAM_PLATFORM_LINUX",
			"CAM_RELEASE"
		}

		-- OpenCV comes from the system (e.g. "sudo apt install libopencv-dev"), so it matches the CPU architecture (x86_64, ARM) of the machine.
		buildoptions { "`pkg-config --cflags opencv4`" }
		linkoptions { "`pkg-config --libs opencv4`" }

		links
		{
			"pthread",
			"anl",
			"ssl",
			"crypto",
			"atomic"
		}

	filter { "system:macos", "configurations:Release" }
		systemversion "latest"
        optimize "On"
		
		defines
		{
			"CAM_PLATFORM_MACOS",
			"CAM_RELEASE"
		}

		links
		{

		}

	

	-- The files, which the update server ships to the cameras (see UpdateServer): the program together with the files it needs. All programs are built into
	-- the same folder (bin/<Configuration>), so the update server must not ship that folder as it is, it would ship all programs.
	filter { "system:windows", "configurations:Debug" }
		postbuildcommands
		{
			("{MKDIR} %{cfg.targetdir}/CamClient-Package"),
			("{COPY} %{cfg.targetdir}/CamClient.exe %{cfg.targetdir}/CamClient-Package"),
			("{COPY} %{cfg.targetdir}/opencv_world4140d.dll %{cfg.targetdir}/CamClient-Package"),
			("{COPY} %{cfg.targetdir}/opencv_videoio_ffmpeg4140_64.dll %{cfg.targetdir}/CamClient-Package"),
		}

	filter { "system:windows", "configurations:Release" }
		postbuildcommands
		{
			("{MKDIR} %{cfg.targetdir}/CamClient-Package"),
			("{COPY} %{cfg.targetdir}/CamClient.exe %{cfg.targetdir}/CamClient-Package"),
			("{COPY} %{cfg.targetdir}/opencv_world4140.dll %{cfg.targetdir}/CamClient-Package"),
			("{COPY} %{cfg.targetdir}/opencv_videoio_ffmpeg4140_64.dll %{cfg.targetdir}/CamClient-Package"),
		}

	filter "system:linux"
		postbuildcommands
		{
			("{MKDIR} %{cfg.targetdir}/CamClient-Package"),
			("{COPY} %{cfg.targetdir}/CamClient %{cfg.targetdir}/CamClient-Package"),
		}
