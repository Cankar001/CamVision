# Face models

The face detection and recognition of the server needs these two model files (ONNX, from the [OpenCV Zoo](https://github.com/opencv/opencv_zoo)). They are not part of the repository. Download them into this folder:

| File | Size | Download |
|---|---|---|
| `face_detection_yunet_2023mar.onnx` (finds the faces, YuNet) | 0.2 MB | https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx |
| `face_recognition_sface_2021dec.onnx` (recognizes who it is, SFace) | 37 MB | https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx |

With only the first file, faces are detected but nobody is recognized. Other locations can be set with `face_detector_model` and `face_recognizer_model` in `server.cfg`.

## Older OpenCV (4.5.x, for example Ubuntu 22.04)

`face_detection_yunet_2023mar.onnx` needs OpenCV 4.8 or newer. With OpenCV 4.5.x every frame fails with `Face analysis failed ... Layer with requested id=-1 not found`. Use the older detector model instead (`face_recognition_sface_2021dec.onnx` works on both):

```bash
wget -O models/face_detection_yunet_2022mar.onnx https://github.com/opencv/opencv_zoo/raw/7e062e54cf5410c09b795ff71b4a255e58498c79/models/face_detection_yunet/face_detection_yunet_2022mar.onnx
```

(The file was removed from the `main` branch of the OpenCV Zoo, so the link points to the last commit that still has it.)

and set `face_detector_model = models/face_detection_yunet_2022mar.onnx` in `server.cfg`. Check the OpenCV version with `pkg-config --modversion opencv4`.
