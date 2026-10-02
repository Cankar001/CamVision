# Face models

The face detection and recognition of the server needs these two model files (ONNX, from the [OpenCV Zoo](https://github.com/opencv/opencv_zoo)). They are not part of the repository. Download them into this folder:

| File | Size | Download |
|---|---|---|
| `face_detection_yunet_2023mar.onnx` (finds the faces, YuNet) | 0.2 MB | https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx |
| `face_recognition_sface_2021dec.onnx` (recognizes who it is, SFace) | 37 MB | https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx |

With only the first file, faces are detected but nobody is recognized. Other locations can be set with `face_detector_model` and `face_recognizer_model` in `server.cfg`.
