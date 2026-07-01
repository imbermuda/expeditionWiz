#pragma once

#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Config.h"

struct LootLine
{
    std::string text;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    float conf = 0.0f;
};

class OCR
{
public:
    OCR() = default;
    ~OCR();

    OCR(const OCR&) = delete;
    OCR& operator=(const OCR&) = delete;

    bool Init(const std::string& tessdataPath);
    void SetupTesseractApi(tesseract::TessBaseAPI& api);
    bool ReinitializeWorkers(const AppConfig& config);

    void SetConfig(const AppConfig* config);

    std::vector<LootLine> RecognizePreparedWithApi(tesseract::TessBaseAPI& api, const cv::Mat& img);
    std::vector<LootLine> RecognizeLoot(const cv::Mat& src, const AppConfig& config);

private:
    bool initialized_ = false;

    const AppConfig* config_ = nullptr;

    struct ThresholdSet
    {
        std::array<double, 6> values{};
        size_t count = 0;
    };

    std::mutex workerMutex_;
    std::condition_variable workerCv_;
    std::condition_variable resultCv_;

    std::string tessdataPath_;
    std::vector<std::unique_ptr<tesseract::TessBaseAPI>> workerApis_;
    std::vector<std::jthread> workerThreads_;
    std::vector<std::vector<LootLine>> workerResults_;

    std::array<double, 6> activeThresholds_{};
    const cv::Mat* activeGray_ = nullptr;
    size_t activePassCount_ = 0;
    size_t pendingWorkers_ = 0;
    uint64_t batchId_ = 0;
    bool stopWorkers_ = false;

private:
    bool CreateWorkers(size_t passes);
    void StopWorkers();
    void WorkerLoop(size_t index);

    static ThresholdSet BuildThresholds(const AppConfig& config);
    static void Trim(std::string& s);
};
