include "./vendor/bin/premake/solution_items.lua"

include "Dependencies.lua"

workspace "CamVision"
    configurations { "Debug", "Release" }

	-- Everything is built into the same two folders: the programs and libraries of all projects into bin/<Configuration>, the intermediate files into
	-- bin-obj/<Configuration>/<Project> (one folder per project, as the files of different projects have the same names). To build from scratch, delete
	-- these two folders. The programs find their project folder (settings, models, ...) from the place of their executable, see Core::Process::EnterProjectFolder.
	targetdir "%{wks.location}/bin/%{cfg.buildcfg}"
	objdir "%{wks.location}/bin-obj/%{cfg.buildcfg}/%{prj.name}"
	debugdir "%{prj.location}"

	solution_items
	{
		".editorconfig"
	}

	-- Newer premake versions replaced the flag by a setting (and removed the flags function), older ones only know the flag.
	if multiprocessorcompile then
		multiprocessorcompile "On"
	elseif flags then
		flags { "MultiProcessorCompile" }
	end

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

	group "Tests"
		include "CamTests"
	group ""

	group "Updater"
		include "UpdateClient"
		include "UpdateServer"
	group ""

