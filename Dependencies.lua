miniz_include_path = path.getabsolute('vendor/miniz/include')
spdlog_include_path = path.getabsolute('vendor/spdlog/include')

IncludeDir = {}
IncludeDir["opencv"]             = "%{wks.location}/CamClient/vendor/opencv/include/"
IncludeDir["cam_core"]           = "%{wks.location}/Cam-Core/src"
IncludeDir["miniz"]        		 = miniz_include_path
IncludeDir["spdlog"]       		 = spdlog_include_path

LibDir = {}
LibDir["opencv_lib_path"]        = "%{wks.location}/CamClient/vendor/opencv/lib"


LibDir["opencv_world_debug"]     = "%{wks.location}/CamClient/vendor/opencv/lib/Debug/opencv_world4140d.lib"

LibDir["opencv_world"]           = "%{wks.location}/CamClient/vendor/opencv/lib/Release/opencv_world4140.lib"

