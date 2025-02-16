**Last HailoRT version checked - 4.14.0**

This is a HailoRT C++ API yolov5seg detection + instance segmentation example.

The example does the following:

Creates a device (pcie)
Reads the network configuration from a yolov5eg HEF file
Prepares the application for inference
Runs inference and postprocess on a given video file
Draws the detection boxes on the original video
Colors the detected objects pixels
Prints the object detected + confidence to the screen
Prints statistics
NOTE: Currently supports only devices connected on a PCIe link.

Prequisites: OpenCV 4.2.X CMake >= 3.20 HailoRT >= 4.10.0 git - rapidjson repository is cloned when performing build.

To compile the example run `./build.sh`

To run the compiled example:


`./build/x86_64/vstream_yolov5seg_example_cpp -hef=YOLOV5SEG_HEF_FILE.hef -input=VIDEO_FILE.mp4`

NOTE: You can also save the processed video by commenting in a few lines at the `post_processing_all` function in yolov5seg_example.cpp.

NOTE: There should be no spaces between "=" given in the command line arguments and the file name itself.
