include "./vendor/bin/premake/solution_items.lua"

-- On Linux the architecture is not part of the name, because it is the one of the machine, which builds it (x86_64, ARM, ...).
if os.target() == "linux" then
	outputdir = "%{cfg.buildcfg}-%{cfg.system}"
else
	outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
end

include "Dependencies.lua"

workspace "CamVision"
    configurations { "Debug", "Release" }

	solution_items
	{
		".editorconfig"
	}

	flags
	{
		"MultiProcessorCompile"
	}

	-- Linux builds for the machine it runs on. Forcing 64 bit x86 there would pass -m64 to the compiler, which does not exist on ARM (Raspberry Pi).
	filter "system:not linux"
		architecture "x64"
	filter {}

	group "Dependencies"
		include "CamClient/vendor/opencv"
		include "vendor/miniz"
		include "vendor/spdlog"
	group ""

	group "Core"
		include "Cam-Core"
	group ""

	group "CamVision"
		include "CamClient"
		include "CamServer"
		include "CamDisplay"
	group ""

	group "Updater"
		include "UpdateClient"
		include "UpdateServer"
	group ""

