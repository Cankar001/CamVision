project "OpenCV"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	
	files
	{
		"include/**.h",
		"include/**.hpp"
	}
	
	includedirs
	{
		"include"
	}
	
	filter "system:linux"
		systemversion "latest"

	filter "system:windows"
		systemversion "latest"
		
	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	-- The DLLs are copied once, into the folder with all programs (bin/<Configuration>). The programs, which use OpenCV, depend on this project.
	filter { "system:windows", "configurations:Debug" }
		postbuildcommands
		{
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_world4140d.dll %{cfg.targetdir}"),
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_videoio_ffmpeg4140_64.dll %{cfg.targetdir}"),
		}

	filter { "system:windows", "configurations:Release" }
		postbuildcommands
		{
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_world4140.dll %{cfg.targetdir}"),
			("{COPY} %{wks.location}CamClient/vendor/opencv/lib/opencv_videoio_ffmpeg4140_64.dll %{cfg.targetdir}"),
		}
