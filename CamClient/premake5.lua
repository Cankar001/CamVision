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
		"%{IncludeDir.opencv}",
    }

    links
    {
        "Cam-Core",
		"spdlog"
    }

	filter { "system:windows", "configurations:Debug" }
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

		libdirs { "%{LibDir.opencv_lib_path_linux}/Debug/" }
		runpathdirs { "%{LibDir.opencv_lib_path_linux}/Debug/" }

		links
		{	
			"pthread",
			"anl",
			"%{LibDir.opencv_linux_core}",
			"%{LibDir.opencv_linux_imgcodecs}",
			"%{LibDir.opencv_linux_imgproc}",
			"%{LibDir.opencv_linux_text}",
			"%{LibDir.opencv_linux_tracking}",
			"%{LibDir.opencv_linux_video}",
			"%{LibDir.opencv_linux_videoio}",
			"%{LibDir.opencv_linux_face}",
			"%{LibDir.opencv_linux_stitching}",
			"%{LibDir.opencv_linux_highgui}"
		}

		postbuildcommands
		{
			("{COPY} %{prj.location}/vendor/opencv/lib-linux/Debug/libopencv_core.so.405 %{cfg.targetdir}"),
			("{COPY} %{prj.location}/vendor/opencv/lib-linux/Debug/libopencv_imgcodecs.so.405 %{cfg.targetdir}"),
			("{COPY} %{prj.location}/vendor/opencv/lib-linux/Debug/libopencv_imgproc.so.405 %{cfg.targetdir}"),
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

		libdirs { "%{LibDir.opencv_lib_path_linux}/Release/" }
		runpathdirs { "%{LibDir.opencv_lib_path_linux}/Release/" }

		links
		{
			"pthread",
			"anl",
			"%{LibDir.opencv_linux_core}",
			"%{LibDir.opencv_linux_imgcodecs}",
			"%{LibDir.opencv_linux_imgproc}",
			"%{LibDir.opencv_linux_text}",
			"%{LibDir.opencv_linux_tracking}",
			"%{LibDir.opencv_linux_video}",
			"%{LibDir.opencv_linux_videoio}",
			"%{LibDir.opencv_linux_face}",
			"%{LibDir.opencv_linux_stitching}",
			"%{LibDir.opencv_linux_highgui}"
		}

		postbuildcommands
		{
			("{COPY} %{prj.location}/vendor/opencv/lib-linux/Release/libopencv_core.so.405 %{cfg.targetdir}"),
			("{COPY} %{prj.location}/vendor/opencv/lib-linux/Release/libopencv_imgcodecs.so.405 %{cfg.targetdir}"),
			("{COPY} %{prj.location}/vendor/opencv/lib-linux/Release/libopencv_imgproc.so.405 %{cfg.targetdir}"),
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

	