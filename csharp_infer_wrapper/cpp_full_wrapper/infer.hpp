#ifndef INFER_H
#define INFER_H

#define INFER_EXPORTS // export

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
    #ifdef INFER_EXPORTS
        #define INFER_API __declspec(dllexport)
    #else
        #define INFER_API __declspec(dllimport)
    #endif
#else
    #define INFER_API
#endif

    INFER_API int infer_wrapper(const char* hef_path, const char* images_path, const char* arch, const float conf_thr, float* detections, const int max_num_detections, int* frames_ready, const int buffer_size);

#ifdef __cplusplus
}
#endif

#endif // INFER_H
