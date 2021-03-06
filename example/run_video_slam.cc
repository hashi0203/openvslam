#ifdef USE_PANGOLIN_VIEWER
#include "pangolin_viewer/viewer.h"
#elif USE_SOCKET_PUBLISHER
#include "socket_publisher/publisher.h"
#endif

#include "openvslam/system.h"
#include "openvslam/config.h"

#include <iostream>
#include <chrono>
#include <numeric>

#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <spdlog/spdlog.h>
#include <popl.hpp>

#ifdef USE_STACK_TRACE_LOGGER
#include <glog/logging.h>
#endif

#ifdef USE_GOOGLE_PERFTOOLS
#include <gperftools/profiler.h>
#endif

void gamma_correction(cv::Mat frame, const int gamma) {
    cv::Mat LookUpTable(1,256,CV_8UC1);
    uchar* p = LookUpTable.data;
    for(int i = 0; i < 256; i++) {
        p[i] = (int)(pow((double)i / 255.0, 1.0 / gamma) * 255.0);
    }
    cv::LUT(frame,LookUpTable,frame);
}

void CLAHE(cv::Mat frame, const float clipLimit, const int tileGridSize) {
    cv::cvtColor(frame,frame,CV_BGR2HSV);

    std::vector<cv::Mat> flame_planes(3);
    cv::split(frame,flame_planes);
    
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
    clahe->setClipLimit(clipLimit);
    clahe->setTilesGridSize(cv::Size(tileGridSize,tileGridSize));
    cv::Mat dst;
    clahe->apply(flame_planes[2],dst);
    
    dst.copyTo(flame_planes[2]);
    cv::merge(flame_planes,frame);

    cv::cvtColor(frame,frame,CV_HSV2BGR);
}

void bilateralFilter(cv::Mat frame, const int d, const int sigmaColor, const int sigmaSpace) {
    cv::Mat dst;
    cv::bilateralFilter(frame,dst,d,sigmaColor,sigmaSpace,cv::BORDER_DEFAULT);
    frame = dst;
}

void nlmf(cv::Mat frame, const float h, const float hColor, const int templateWindowSize, const int searchWindowSize) {
    cv::Mat dst;
    cv::fastNlMeansDenoisingColored(frame,dst,h,hColor,templateWindowSize,searchWindowSize);
    frame = dst;
}

void filter2D(cv::Mat frame) {
    cv::Mat kernel = (cv::Mat_<int>(3,3) << 0,-1,0,-1,5,-1,0,-1,0);
    cv::filter2D(frame, frame, -1, kernel, cv::Point(-1, -1), 0, cv::BORDER_DEFAULT);
}

void mono_tracking(const std::shared_ptr<openvslam::config>& cfg,
                   const std::string& vocab_file_path, const std::string& video_file_path, const std::string& mask_img_path,
                   const unsigned int frame_skip, const bool no_sleep, const bool auto_term,
                   const bool eval_log, const std::string& map_db_path, const int process_option) {
    // load the mask image
    const cv::Mat mask = mask_img_path.empty() ? cv::Mat{} : cv::imread(mask_img_path, cv::IMREAD_GRAYSCALE);

    // build a SLAM system
    openvslam::system SLAM(cfg, vocab_file_path);
    // startup the SLAM process
    SLAM.startup();

    // create a viewer object
    // and pass the frame_publisher and the map_publisher
#ifdef USE_PANGOLIN_VIEWER
    pangolin_viewer::viewer viewer(cfg, &SLAM, SLAM.get_frame_publisher(), SLAM.get_map_publisher());
#elif USE_SOCKET_PUBLISHER
    socket_publisher::publisher publisher(cfg, &SLAM, SLAM.get_frame_publisher(), SLAM.get_map_publisher());
#endif

    auto video = cv::VideoCapture(video_file_path, cv::CAP_FFMPEG);
    std::vector<double> track_times;

    cv::Mat frame;
    double timestamp = 0.0;

    unsigned int num_frame = 0;

    bool is_not_end = true;
    // run the SLAM in another thread
    std::thread thread([&]() {
        std::chrono::system_clock::time_point  start, end; // 型は auto で可
        start = std::chrono::system_clock::now(); // 計測開始時間

        while (is_not_end) {
            is_not_end = video.read(frame);

            const auto tp_1 = std::chrono::steady_clock::now();

            if (!frame.empty() && (num_frame % frame_skip == 0)) {
                // input the current frame and estimate the camera pose

                if (process_option == 1) {
                    gamma_correction(frame,2.0);
                    bilateralFilter(frame,15,20,20);
                    CLAHE(frame,2.0,7);
                    bilateralFilter(frame,15,20,20);
                } else if (process_option == 2) {
                    CLAHE(frame,2.0,7);
                    nlmf(frame,10,10,7,21);
                } else if (process_option == 3) {
                    filter2D(frame);
                }

                SLAM.feed_monocular_frame(frame, timestamp, mask);
            }

            const auto tp_2 = std::chrono::steady_clock::now();

            const auto track_time = std::chrono::duration_cast<std::chrono::duration<double>>(tp_2 - tp_1).count();
            if (num_frame % frame_skip == 0) {
                track_times.push_back(track_time);
            }

            // wait until the timestamp of the next frame
            if (!no_sleep) {
                const auto wait_time = 1.0 / cfg->camera_->fps_ - track_time;
                if (0.0 < wait_time) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<unsigned int>(wait_time * 1e6)));
                }
            }

            timestamp += 1.0 / cfg->camera_->fps_;
            ++num_frame;

            // check if the termination of SLAM system is requested or not
            if (SLAM.terminate_is_requested()) {
                break;
            }
        }

        end = std::chrono::system_clock::now();  // 計測終了時間
        double elapsed = std::chrono::duration_cast<std::chrono::seconds>(end-start).count(); //処理に要した時間をミリ秒に変換
        std::cout << "elapsed time: " << elapsed << "[s]" << std::endl;

        // wait until the loop BA is finished
        while (SLAM.loop_BA_is_running()) {
            std::this_thread::sleep_for(std::chrono::microseconds(5000));
        }

        // automatically close the viewer
#ifdef USE_PANGOLIN_VIEWER
        if (auto_term) {
            viewer.request_terminate();
        }
#elif USE_SOCKET_PUBLISHER
        if (auto_term) {
            publisher.request_terminate();
        }
#endif
    });

    // run the viewer in the current thread
#ifdef USE_PANGOLIN_VIEWER
    viewer.run();
#elif USE_SOCKET_PUBLISHER
    publisher.run();
#endif

    thread.join();

    // shutdown the SLAM process
    SLAM.shutdown();

    if (eval_log) {
        // output the trajectories for evaluation
        SLAM.save_frame_trajectory("frame_trajectory.txt", "TUM");
        SLAM.save_keyframe_trajectory("keyframe_trajectory.txt", "TUM");
        // output the tracking times for evaluation
        std::ofstream ofs("track_times.txt", std::ios::out);
        if (ofs.is_open()) {
            for (const auto track_time : track_times) {
                ofs << track_time << std::endl;
            }
            ofs.close();
        }
    }

    if (!map_db_path.empty()) {
        // output the map database
        SLAM.save_map_database(map_db_path);
    }

    std::sort(track_times.begin(), track_times.end());
    const auto total_track_time = std::accumulate(track_times.begin(), track_times.end(), 0.0);
    std::cout << "median tracking time: " << track_times.at(track_times.size() / 2) << "[s]" << std::endl;
    std::cout << "mean tracking time: " << total_track_time / track_times.size() << "[s]" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef USE_STACK_TRACE_LOGGER
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
#endif

    // create options
    popl::OptionParser op("Allowed options");
    auto help = op.add<popl::Switch>("h", "help", "produce help message");
    auto vocab_file_path = op.add<popl::Value<std::string>>("v", "vocab", "vocabulary file path");
    auto video_file_path = op.add<popl::Value<std::string>>("m", "video", "video file path");
    auto config_file_path = op.add<popl::Value<std::string>>("c", "config", "config file path");
    auto mask_img_path = op.add<popl::Value<std::string>>("", "mask", "mask image path", "");
    auto frame_skip = op.add<popl::Value<unsigned int>>("", "frame-skip", "interval of frame skip", 1);
    auto no_sleep = op.add<popl::Switch>("", "no-sleep", "not wait for next frame in real time");
    auto auto_term = op.add<popl::Switch>("", "auto-term", "automatically terminate the viewer");
    auto debug_mode = op.add<popl::Switch>("", "debug", "debug mode");
    auto eval_log = op.add<popl::Switch>("", "eval-log", "store trajectory and tracking times for evaluation");
    auto map_db_path = op.add<popl::Value<std::string>>("p", "map-db", "store a map database at this path after SLAM", "");
    auto process_option = op.add<popl::Value<unsigned int>>("", "proc-opt", "process option", 0);
    try {
        op.parse(argc, argv);
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << std::endl;
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }

    // check validness of options
    if (help->is_set()) {
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }
    if (!vocab_file_path->is_set() || !video_file_path->is_set() || !config_file_path->is_set()) {
        std::cerr << "invalid arguments" << std::endl;
        std::cerr << std::endl;
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }

    // setup logger
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%L] %v%$");
    if (debug_mode->is_set()) {
        spdlog::set_level(spdlog::level::debug);
    }
    else {
        spdlog::set_level(spdlog::level::info);
    }

    // load configuration
    std::shared_ptr<openvslam::config> cfg;
    try {
        cfg = std::make_shared<openvslam::config>(config_file_path->value());
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

#ifdef USE_GOOGLE_PERFTOOLS
    ProfilerStart("slam.prof");
#endif

    // run tracking
    if (cfg->camera_->setup_type_ == openvslam::camera::setup_type_t::Monocular) {
        mono_tracking(cfg, vocab_file_path->value(), video_file_path->value(), mask_img_path->value(),
                      frame_skip->value(), no_sleep->is_set(), auto_term->is_set(),
                      eval_log->is_set(), map_db_path->value(), process_option->value());
    }
    else {
        throw std::runtime_error("Invalid setup type: " + cfg->camera_->get_setup_type_string());
    }

#ifdef USE_GOOGLE_PERFTOOLS
    ProfilerStop();
#endif

    return EXIT_SUCCESS;
}
