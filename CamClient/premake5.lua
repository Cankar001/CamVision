project "CamClient"
    kind "ConsoleApp"
    language "C++"
	cppdialect "C++17"
	staticruntime "off"
	entrypoint "mainCRTStartup"
	
	dependson
	{
		"Cam-Core"
	}

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    debugdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-obj/" .. outputdir .. "/%{prj.name}")

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

		postbuildcommands
		{	
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_world4140d.dll %{cfg.targetdir}"),
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_world4140.dll %{cfg.targetdir}"),
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_videoio_ffmpeg4140_64.dll %{cfg.targetdir}"),
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
			"crypto"
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
		
		postbuildcommands
		{	
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_world4140d.dll %{cfg.targetdir}"),
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_world4140.dll %{cfg.targetdir}"),
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_videoio_ffmpeg4140_64.dll %{cfg.targetdir}"),
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
			"crypto"
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

	