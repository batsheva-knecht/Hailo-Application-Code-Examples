#include "infer.hpp"
#include "hailo/hailort.hpp"
#include "common.hpp"
#include "yolo_post_processing.hpp"

#include <iostream>
#include <chrono>
#include <mutex>
#include <future>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/core/matx.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef _WIN32
    #include <Windows.h>
#endif

using namespace hailort;

constexpr bool QUANTIZED = true;
constexpr hailo_format_type_t FORMAT_TYPE = HAILO_FORMAT_TYPE_AUTO;

void print_inference_statistics(std::chrono::duration<double> inference_time,
                                std::chrono::duration<double> postprocess_time, 
                                std::string hef_file, double frame_count) 
{
    std::cout << BOLDGREEN << "\n-I-----------------------------------------------" << std::endl;
    std::cout << "-I- " << hef_file.substr(0, hef_file.find(".")) << std::endl;
    std::cout << "-I-----------------------------------------------" << std::endl;
    std::cout << "\n-I-----------------------------------------------" << std::endl;
    std::cout << "-I- Inference                                    " << std::endl;
    std::cout << "-I-----------------------------------------------" << std::endl;
    std::cout << "-I- Total time:   " << inference_time.count() << " sec" << std::endl;
    std::cout << "-I- Average FPS:  " << frame_count / (inference_time.count()) << std::endl;
    std::cout << "-I- Latency:      " << 1.0 / (frame_count / (inference_time.count()) / 1000) << " ms" << std::endl;
    std::cout << "-I-----------------------------------------------" << std::endl;
    std::cout << "\n-I-----------------------------------------------" << std::endl;
    std::cout << "-I- Postprocess                                    " << std::endl;
    std::cout << "-I-----------------------------------------------" << std::endl;
    std::cout << "-I- Total time:   " << postprocess_time.count() << " sec" << std::endl;
    std::cout << "-I- Average FPS:  " << frame_count / (postprocess_time.count()) << std::endl;
    std::cout << "-I- Latency:      " << 1.0 / (frame_count / (postprocess_time.count()) / 1000) << " ms" << std::endl;
    std::cout << "-I-----------------------------------------------" << std::endl << RESET;
}

inline bool ends_with(std::string const & value, std::string const & ending) 
{
    if (ending.size() > value.size()) return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}


hailo_status post_processing_all(std::vector<std::shared_ptr<FeatureData>> &features, 
                                std::chrono::duration<double>& postprocess_time, std::vector<cv::Mat>& frames, int frame_count, std::string arch,
                                float32_t* detections, const int max_num_detections, int* frames_ready, const int buffer_size, float thr)
{
    std::sort(features.begin(), features.end(), &FeatureData::sort_tensors_by_size);
    auto t_start = std::chrono::high_resolution_clock::now(); 
    for (int idx_frame = 0; idx_frame < frame_count; idx_frame++) {
        auto detections_struct = post_processing(max_num_detections, thr, arch,
            features[0]->m_buffers.get_read_buffer().data(), features[0]->m_qp_zp, features[0]->m_qp_scale,
            features[1]->m_buffers.get_read_buffer().data(), features[1]->m_qp_zp, features[1]->m_qp_scale,
            features[2]->m_buffers.get_read_buffer().data(), features[2]->m_qp_zp, features[2]->m_qp_scale);
    
        for (auto &feature : features) {
            feature->m_buffers.release_read_buffer();
        }

        int num_detections = 0;
        int detection_size = 6;
        int idx_buffer = idx_frame % buffer_size;
        size_t detections_4_byte_idx = idx_buffer * detection_size * max_num_detections;
        // If you want to check that c# consumed the old detections, check that frames_ready[idx_buffer] == -1 before writing in detections array
        for (auto& detection : detections_struct) {
            if (detection.confidence >= thr && num_detections < max_num_detections) { 
    
                detections[detections_4_byte_idx++] = detection.ymin;
                detections[detections_4_byte_idx++] = detection.xmin;
                detections[detections_4_byte_idx++] = detection.ymax;
                detections[detections_4_byte_idx++] = detection.xmax;
                detections[detections_4_byte_idx++] = detection.confidence;
                detections[detections_4_byte_idx++] = static_cast<float32_t>(detection.class_id);

                num_detections++;
            }
        }
        
        frames_ready[idx_buffer] = num_detections; // indicates (to c#) that we have finished processing frame idx_buffer, and found num_detections detections.
        frames[idx_frame].release();   
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    postprocess_time = t_end - t_start;

    return HAILO_SUCCESS;
}


hailo_status read_all(OutputVStream& output_vstream, std::shared_ptr<FeatureData> feature, int frame_count, std::chrono::steady_clock::time_point& read_time_vec) 
{
    hailo_status status = HAILO_UNINITIALIZED;
    for (int i = 0; i < frame_count; i++) {
        auto& buffer = feature->m_buffers.get_write_buffer();
        status = output_vstream.read(MemoryView(buffer.data(), buffer.size()));
        feature->m_buffers.release_write_buffer();
        if (HAILO_SUCCESS != status) {
            std::cerr << "Failed reading with status = " <<  status << std::endl;
            return status;
        }
    }

    read_time_vec = std::chrono::high_resolution_clock::now();
    return HAILO_SUCCESS;
}

hailo_status write_all(InputVStream& input_vstream, std::string images_path, std::chrono::steady_clock::time_point& write_time_vec, std::vector<cv::Mat>& frames) 
{
    hailo_status status = HAILO_UNINITIALIZED;
    
    auto input_shape = input_vstream.get_info().shape;
    int height = input_shape.height;
    int width = input_shape.width;

    std::vector<cv::String> file_names;
    cv::glob(images_path, file_names, false);

    write_time_vec = std::chrono::high_resolution_clock::now();
    int idx_frame = 0;
    for (std::string file : file_names) {
        if (ends_with(file, ".jpg") || ends_with(file, ".png") || ends_with(file, ".jpeg")) {
            frames[idx_frame] = cv::imread(file,  cv::IMREAD_COLOR);
            if (frames[idx_frame].channels() == 3) {
                cv::cvtColor(frames[idx_frame], frames[idx_frame], cv::COLOR_BGR2RGB);
            }
            if (frames[idx_frame].rows != height || frames[idx_frame].cols != width) {
                cv::resize(frames[idx_frame], frames[idx_frame], cv::Size(width, height), cv::INTER_AREA);
            }
            status = input_vstream.write(MemoryView(frames[idx_frame].data, input_vstream.get_frame_size()));
            if (HAILO_SUCCESS != status) {
                return status;
            }
        }
    }
    return HAILO_SUCCESS;
}


hailo_status create_feature(hailo_vstream_info_t vstream_info, size_t output_frame_size, std::shared_ptr<FeatureData> &feature) 
{
    hailo_status status = HAILO_UNINITIALIZED;
    feature = std::make_shared<FeatureData>(static_cast<uint32_t>(output_frame_size), vstream_info.quant_info.qp_zp,
        vstream_info.quant_info.qp_scale, vstream_info.shape.width);
    if (!feature) {
        status = HAILO_OUT_OF_HOST_MEMORY;
        return status;
    }
    return HAILO_SUCCESS;
}


hailo_status run_inference(std::vector<InputVStream>& input_vstream, std::vector<OutputVStream>& output_vstreams, std::string images_path, int frame_count,
                    std::chrono::steady_clock::time_point& write_time_vec,
                    std::vector<std::chrono::steady_clock::time_point>& read_time_vec,
                    std::chrono::duration<double>& inference_time, std::chrono::duration<double>& postprocess_time, std::string arch, const float conf_thr,
                    float32_t* detections, const int max_num_detections, int* frames_ready, const int buffer_size) 
{

    hailo_status status = HAILO_UNINITIALIZED;
    
    auto output_vstreams_size = output_vstreams.size();

    std::vector<std::shared_ptr<FeatureData>> features;
    features.reserve(output_vstreams_size);
    for (size_t i = 0; i < output_vstreams_size; i++) {
        std::shared_ptr<FeatureData> feature(nullptr);
        status = create_feature(output_vstreams[i].get_info(), output_vstreams[i].get_frame_size(), feature);
        if (HAILO_SUCCESS != status) {
            std::cerr << "Failed creating feature with status = " << status << std::endl;
            return status;
        }

        features.emplace_back(feature);
    }

    std::vector<cv::Mat> frames(static_cast<size_t>(frame_count));

    auto input_thread(std::async(write_all, std::ref(input_vstream[0]), images_path, std::ref(write_time_vec), std::ref(frames)));

    // Create read threads
    std::vector<std::future<hailo_status>> output_threads;
    output_threads.reserve(output_vstreams_size);
    for (size_t i = 0; i < output_vstreams_size; i++) {
        output_threads.emplace_back(std::async(read_all, std::ref(output_vstreams[i]), features[i], frame_count, std::ref(read_time_vec[i])));
    }

    auto pp_thread(std::async(post_processing_all, std::ref(features), std::ref(postprocess_time), std::ref(frames), frame_count, arch, detections, max_num_detections, 
        frames_ready, buffer_size, conf_thr));

    for (size_t i = 0; i < output_threads.size(); i++) {
        status = output_threads[i].get();
        if (HAILO_SUCCESS != status) {
            std::cerr << "Read failed with status " << status << std::endl;
            return status;
        }
    }
    auto input_status = input_thread.get();
    auto pp_status = pp_thread.get();

    if (HAILO_SUCCESS != input_status) {
        std::cerr << "Write thread failed with status " << input_status << std::endl;
        return input_status; 
    }
    if (HAILO_SUCCESS != pp_status) {
        std::cerr << "Post-processing failed with status " << pp_status << std::endl;
        return pp_status;
    }

    inference_time = read_time_vec[0] - write_time_vec;
    for (size_t i = 1; i < output_vstreams.size(); i++){
        if (inference_time.count() < (double)(read_time_vec[i] - write_time_vec).count())
            inference_time = read_time_vec[i] - write_time_vec;
    }

    return HAILO_SUCCESS;
}

void print_net_banner(std::pair<std::vector<hailort::InputVStream>, std::vector<hailort::OutputVStream>> &vstreams) 
{
    std::cout << BOLDMAGENTA << "-I-----------------------------------------------" << std::endl << RESET;
    std::cout << BOLDMAGENTA << "-I-  Network  Name                                     " << std::endl << RESET;
    std::cout << BOLDMAGENTA << "-I-----------------------------------------------" << std::endl << RESET;
    for (auto const& value: vstreams.first) {
        std::cout << MAGENTA << "-I-  IN:  " << value.name() <<std::endl << RESET;
    }
    std::cout << BOLDMAGENTA << "-I-----------------------------------------------" << std::endl << RESET;
    for (auto const& value: vstreams.second) {
        std::cout << MAGENTA << "-I-  OUT: " << value.name() <<std::endl << RESET;
    }
    std::cout << BOLDMAGENTA << "-I-----------------------------------------------" << RESET << std::endl;
}

Expected<std::shared_ptr<ConfiguredNetworkGroup>> configure_network_group(VDevice &vdevice, std::string hef_path)
{
    auto hef_exp = Hef::create(hef_path);
    if (!hef_exp) {
        return make_unexpected(hef_exp.status());
    }
    auto hef = hef_exp.release();

    auto configure_params = hef.create_configure_params(HAILO_STREAM_INTERFACE_PCIE);
    if (!configure_params) {
        return make_unexpected(configure_params.status());
    }

    auto network_groups = vdevice.configure(hef, configure_params.value());
    if (!network_groups) {
        return make_unexpected(network_groups.status());
    }

    if (1 != network_groups->size()) {
        std::cerr << "Invalid amount of network groups" << std::endl;
        return make_unexpected(HAILO_INTERNAL_FAILURE);
    }

    return std::move(network_groups->at(0));
}

// extern "C" 
int infer_wrapper(const char* hef_path, const char* images_path, const char* arch, const float conf_thr, float* detections, const int max_num_detections, int* frames_ready, const int buffer_size) 
{
    auto infer_start = std::chrono::steady_clock::now();

    hailo_status status = HAILO_UNINITIALIZED;

    std::chrono::duration<double> total_time;
    std::chrono::steady_clock::time_point t_start = std::chrono::high_resolution_clock::now();

    std::chrono::steady_clock::time_point write_time_vec;
    std::chrono::duration<double> inference_time;
    std::chrono::duration<double> postprocess_time;

    auto vdevice_exp = VDevice::create();
    if (!vdevice_exp) {
        std::cerr << "Failed create vdevice, status = " << vdevice_exp.status() << std::endl;
        return vdevice_exp.status();
    }
    auto vdevice = vdevice_exp.release();

    auto network_group_exp = configure_network_group(*vdevice, hef_path);
    if (!network_group_exp) {
        std::cerr << "Failed to configure network group " << hef_path << std::endl;
        return network_group_exp.status();
    }
    auto network_group = network_group_exp.release();

    // quantized=true for input vstreams is ok for this specific yolov5 network, where qp_zp=0, qp_scale=1. In general, use for input vstreams quantized=false.
    auto vstreams_exp = VStreamsBuilder::create_vstreams(*network_group, QUANTIZED, FORMAT_TYPE);
    if (!vstreams_exp) {
        std::cerr << "Failed creating vstreams " << vstreams_exp.status() << std::endl;
        return vstreams_exp.status();
    }
    auto vstreams = vstreams_exp.release();

    std::vector<std::chrono::steady_clock::time_point> read_time_vec(vstreams.second.size());

    print_net_banner(vstreams);

    std::vector<cv::String> file_names;
    cv::glob(images_path, file_names, false);
    int frame_count = 0;
    for (auto& file : file_names) {
        if (ends_with(file, ".jpg") || ends_with(file, ".png") || ends_with(file, ".jpeg")) {
            frame_count++;
        }
    }

    status = run_inference(std::ref(vstreams.first), 
                        std::ref(vstreams.second), 
                        images_path, frame_count, write_time_vec, read_time_vec, 
                        inference_time, postprocess_time, arch, conf_thr,
                        detections, max_num_detections, 
                        frames_ready, buffer_size);

    if (HAILO_SUCCESS != status) {
        std::cerr << "Failed running inference with status = " << status << std::endl;
        return status;
    }

    // print_inference_statistics(inference_time, postprocess_time, hef_path, frame_count);

    std::chrono::steady_clock::time_point t_end = std::chrono::high_resolution_clock::now();
    total_time = t_end - t_start;

    // std::cout << BOLDBLUE << "\n-I- Inference run finished successfully" << RESET << std::endl;
    auto infer_end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(infer_end - infer_start);
    
    std::cout << "FPS cpp " << (frame_count*1000)/duration.count() << std::endl; // 1000: millisecs to secs
    status = HAILO_SUCCESS;
    return status;
}

#ifdef _WIN32
    BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID)
    {
        switch (ul_reason_for_call)
        {
            case DLL_PROCESS_ATTACH:
            case DLL_THREAD_ATTACH:
            case DLL_THREAD_DETACH:
            case DLL_PROCESS_DETACH:
                break;
        }
        return TRUE;
    }
#endif
