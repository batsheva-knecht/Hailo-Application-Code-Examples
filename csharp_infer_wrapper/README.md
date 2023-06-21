
### Full Yolov5 Inference On Hailo-8 With c# Wrapper
## This branch is for windows (MSVC compiler)

## Usage  
0. Get hef: https://hailo-model-zoo.s3.eu-west-2.amazonaws.com/ModelZoo/Compiled/v2.4.0/yolov5m_wo_spp_60p.hef  
and move it to `Hailo-Application-Code-Examples\csharp_infer_wrapper\infer_wrapper`.  
1. Build & run cpp_full_wrapper in Release mode (in vscode).  
Another way to do it is in vs terminal:  
```
cd csharp_infer_wrapper\cpp_full_wrapper
cmake -H. -Bbuild -A=x64 -DCMAKE_BUILD_TYPE=Release && cmake --build build --config release
``` 
2. Build & run infer_wrapper (c#) in Release mode  
One way to do it is in vs terminal (x64 Native Tools Command Prompt for VS):  
```
cd ..\infer_wrapper
dotnet build -c Release
dotnet run
```  

## Prerequirements   
1. OpenCV 4.2.X   
2. CMake >= 3.0.0   
3. HailoRT   

## How Does It Works?   
`Program.cs` (c# wrapper) creates a buffer for the detections named `detections`, of size: `DETECTION_SIZE x MAX_NUM_DETECTIONS x BUFFER_SIZE`.  
Another array `frames_ready` is created of size `BUFFER_SIZE`.  
Both arrays are sent to `infer.dll` (c++), and after each frame is processed, the c++ part will update the detections in `detections`.  
To indicate to the c# part that it has finished processing the frame `frame_idx` and that the detections are ready,  
the c++ part will also update `frames_ready` in index `frame_idx` with `num_detections_found`.  
After consuming the detections in the c# part, it will update `frames_ready` in index `frame_idx` with `-1`.  
Note that the c++ part does not check if the detections were 'consumed' (read by the c# part):  
Let's say that `BUFFER_SIZE` is 2, and that `MAX_NUM_DETECTIONS` is 1 (for simplicity). The c++ part produces detections for frame 0, and then detections for frame 1.  
Now both index 0 and index 1 in `detections` are used. If the c# part didn't consume yet the detections for frame 0, the c++ part won't wait and will write the detections for frame 3 at index 0.  
This was chosen under the assumption that the consumption of the detections (c#) is faster than their production (writing to the buffer by c++), and in order to not affect performance.  
It can be easily changed by checking `frames_ready[idx_buffer] == -1` before writing the detections, at `post_processing_all` function in `infer.cpp`.  
## More Info   
- Run full yolov5 inference on hailo-8 including post processing (nms)   
- infer.dll get path to images folder (jpg / png / jpeg), and return the detections found   
- For better FPS performance, use images in resolution 640x640    
- This solution was tailored for Linux OS, but it will work on Windows too.  
  However, when using Windows, it would be better to use C++/CLI which enables writing c# and c++ code together,  
  or to use a buffer with shared named_mutex and named_condition_variable for synchronization.  
