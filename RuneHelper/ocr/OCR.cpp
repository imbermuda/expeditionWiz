#include "OCR.h"

#include "core/Logger.h"
#include "ocr/NameNormalizer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

OCR::~OCR()
{
    StopWorkers();
}

bool OCR::Init(const std::string& tessdataPath)
{
    LOG_INFO("OCR::Init tessdataPath = " + tessdataPath);

    tessdataPath_ = tessdataPath;
    std::filesystem::path engPath = std::filesystem::path(tessdataPath) / "eng.traineddata";

    if (!std::filesystem::exists(engPath))
    {
        LOG_ERROR("eng.traineddata not found: " + engPath.string());
        return false;
    }

    int passes = config_ ? config_->ocrPasses : 1;
    passes = std::clamp(passes, 1, 6);

    if (!CreateWorkers(static_cast<size_t>(passes)))
        return false;

    initialized_ = true;
    LOG_INFO("OCR initialized");

    return true;
}

void OCR::SetupTesseractApi(tesseract::TessBaseAPI& api)
{
    api.SetPageSegMode(tesseract::PSM_SINGLE_BLOCK);

    api.SetVariable(
        "tessedit_char_whitelist",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        " '-"
    );

    api.SetVariable("preserve_interword_spaces", "1");
}

void OCR::SetConfig(const AppConfig* config)
{
    config_ = config;
}

bool OCR::ReinitializeWorkers(const AppConfig& config)
{
    int passes = std::clamp(config.ocrPasses, 1, 6);
    return CreateWorkers(static_cast<size_t>(passes));
}

bool OCR::CreateWorkers(size_t passes)
{
    StopWorkers();

    std::vector<std::unique_ptr<tesseract::TessBaseAPI>> apis;
    apis.reserve(passes);

    for (size_t i = 0; i < passes; ++i)
    {
        auto api = std::make_unique<tesseract::TessBaseAPI>();
        int rc = api->Init(tessdataPath_.c_str(), "eng", tesseract::OEM_LSTM_ONLY);
        if (rc != 0)
        {
            LOG_ERROR("Tesseract worker api.Init failed, rc=" + std::to_string(rc));
            return false;
        }

        SetupTesseractApi(*api);
        apis.push_back(std::move(api));
    }

    {
        std::lock_guard lock(workerMutex_);
        stopWorkers_ = false;
        activeGray_ = nullptr;
        activePassCount_ = 0;
        pendingWorkers_ = 0;
        ++batchId_;

        workerApis_ = std::move(apis);
        workerResults_.clear();
        workerResults_.resize(workerApis_.size());
        workerThreads_.clear();
        workerThreads_.reserve(workerApis_.size());

        for (size_t i = 0; i < workerApis_.size(); ++i)
            workerThreads_.emplace_back([this, i](std::stop_token) { WorkerLoop(i); });
    }

    return true;
}

void OCR::StopWorkers()
{
    {
        std::lock_guard lock(workerMutex_);
        stopWorkers_ = true;
        pendingWorkers_ = 0;
        activeGray_ = nullptr;
        activePassCount_ = 0;
        ++batchId_;
    }

    workerCv_.notify_all();
    resultCv_.notify_all();
    workerThreads_.clear();

    {
        std::lock_guard lock(workerMutex_);
        workerApis_.clear();
        workerResults_.clear();
        stopWorkers_ = false;
    }
}

void OCR::WorkerLoop(size_t index)
{
    uint64_t seenBatch = 0;

    while (true)
    {
        const cv::Mat* gray = nullptr;
        double threshold = 0.0;
        uint64_t batch = 0;

        {
            std::unique_lock lock(workerMutex_);
            workerCv_.wait(lock, [&]
                {
                    return stopWorkers_ || batchId_ != seenBatch;
                });

            if (stopWorkers_)
                return;

            seenBatch = batchId_;
            if (index >= activePassCount_ || !activeGray_)
                continue;

            gray = activeGray_;
            threshold = activeThresholds_[index];
            batch = seenBatch;
        }

        cv::Mat prepared;
        cv::threshold(*gray, prepared, threshold, 255, cv::THRESH_BINARY);
        auto result = RecognizePreparedWithApi(*workerApis_[index], prepared);

        {
            std::lock_guard lock(workerMutex_);
            if (batch != batchId_ || index >= workerResults_.size())
                continue;

            workerResults_[index] = std::move(result);
            if (pendingWorkers_ > 0)
                --pendingWorkers_;

            if (pendingWorkers_ == 0)
                resultCv_.notify_one();
        }
    }
}

OCR::ThresholdSet OCR::BuildThresholds(const AppConfig& config)
{
    int passes = config.ocrPasses;

    switch (passes)
    {
    case 1:
        return { { 130.0 }, 1 };

    case 2:
        return { { 60.0, 130.0 }, 2 };

    case 3:
        return { { 30.0, 60.0, 130.0 }, 3 };

    case 4:
        return { { 30.0, 60.0, 130.0, 180.0 }, 4 };

    case 5:
        return { { 20.0, 30.0, 60.0, 130.0, 180.0 }, 5 };

    case 6:
        return { { 20.0, 30.0, 60.0, 130.0, 180.0, 220.0 }, 6 };

    default:
        return { { 130.0 }, 1 };
    }
}

std::vector<LootLine> OCR::RecognizeLoot(const cv::Mat& img, const AppConfig& config)
{
    if (!initialized_ || img.empty())
        return {};

    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);

    auto thresholds = BuildThresholds(config);
    if (thresholds.count == 0)
        return {};

    std::unique_lock lock(workerMutex_);

    const size_t passCount = (std::min)(thresholds.count, workerApis_.size());
    if (passCount == 0)
        return {};

    if (passCount == 1)
    {
        cv::Mat prepared;
        cv::threshold(gray, prepared, thresholds.values[0], 255, cv::THRESH_BINARY);
        return RecognizePreparedWithApi(*workerApis_[0], prepared);
    }

    for (size_t i = 0; i < passCount; ++i)
        activeThresholds_[i] = thresholds.values[i];

    activeGray_ = &gray;
    activePassCount_ = passCount;
    pendingWorkers_ = passCount;
    workerResults_.clear();
    workerResults_.resize(passCount);
    ++batchId_;

    workerCv_.notify_all();
    resultCv_.wait(lock, [this]
        {
            return pendingWorkers_ == 0 || stopWorkers_;
        });

    activeGray_ = nullptr;
    activePassCount_ = 0;

    std::vector<LootLine> best;

    for (auto& result : workerResults_)
    {
        if (result.size() > best.size())
            best = std::move(result);
    }

    return best;
}

std::vector<LootLine> OCR::RecognizePreparedWithApi(tesseract::TessBaseAPI& api, const cv::Mat& img)
{
    api.SetImage( img.data, img.cols, img.rows, 1, static_cast<int>(img.step));
    api.Recognize(nullptr);

    std::vector<LootLine> result;

    tesseract::ResultIterator* ri = api.GetIterator();
    if (!ri)
        return result;

    do
    {
        char* text = ri->GetUTF8Text(tesseract::RIL_TEXTLINE);
        if (!text)
            continue;

        std::string line(text);
        delete[] text;

        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());

        Trim(line);

        if (line.empty())
            continue;

        float conf = ri->Confidence(tesseract::RIL_TEXTLINE);
        if (conf < 35.0f)
            continue;

        int x1 = 0;
        int y1 = 0;
        int x2 = 0;
        int y2 = 0;

        if (!ri->BoundingBox(tesseract::RIL_TEXTLINE, &x1, &y1, &x2, &y2))
            continue;

        result.push_back({line, x1, y1, x2, y2,conf});

    } while (ri->Next(tesseract::RIL_TEXTLINE));

    return result;
}

void OCR::Trim(std::string& s)
{
    s.erase(
        s.begin(),
        std::find_if(
            s.begin(),
            s.end(),
            [](unsigned char c)
            {
                return !std::isspace(c);
            }));

    s.erase(
        std::find_if(
            s.rbegin(),
            s.rend(),
            [](unsigned char c)
            {
                return !std::isspace(c);
            }).base(),
                s.end());
}
