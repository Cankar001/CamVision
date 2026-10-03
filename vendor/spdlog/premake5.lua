project "spdlog"
	kind "StaticLib"
	language "C++"
	staticruntime "off"


	files
	{
		"src/**.cpp",
	}

	includedirs
	{
		"include"
	}
		
	defines
	{
		"SPDLOG_COMPILED_LIB"
	}

	filter "system:windows"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
	