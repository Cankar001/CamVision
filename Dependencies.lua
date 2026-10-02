miniz_include_path = path.getabsolute('vendor/miniz/include')
spdlog_include_path = path.getabsolute('vendor/spdlog/include')

IncludeDir = {}
IncludeDir["opencv"]             = "%{wks.location}/CamClient/vendor/opencv/include/"
IncludeDir["cam_core"]           = "%{wks.location}/Cam-Core/src"
IncludeDir["miniz"]        		 = miniz_include_path
IncludeDir["spdlog"]       		 = spdlog_include_path

LibDir = {}
LibDir["opencv_lib_path"]        = "%{wks.location}/CamClient/vendor/opencv/lib"
LibDir["opencv_lib_path_linux"]  = "%{wks.location}/CamClient/vendor/opencv/lib-linux"


LibDir["opencv_world_debug"]     = "%{wks.location}/CamClient/vendor/opencv/lib/Debug/opencv_world4140d.lib"

LibDir["opencv_world"]           = "%{wks.location}/CamClient/vendor/opencv/lib/Release/opencv_world4140.lib"

LibDir["opencv_linux_core"]      = "opencv_core"
LibDir["opencv_linux_imgcodecs"] = "opencv_imgcodecs"
LibDir["opencv_linux_imgproc"]   = "opencv_imgproc"
LibDir["opencv_linux_text"]      = "opencv_text"
LibDir["opencv_linux_tracking"]  = "opencv_tracking"
LibDir["opencv_linux_video"]     = "opencv_video"
LibDir["opencv_linux_videoio"]   = "opencv_videoio"
LibDir["opencv_linux_face"]      = "opencv_face"
LibDir["opencv_linux_stitching"] = "opencv_stitching"
LibDir["opencv_linux_highgui"]   = "opencv_highgui"
