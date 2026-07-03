#include "core/OcrService.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <thread>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "core/Helpers.h"
#include "core/Logger.h"
#include "ocr/LootParser.h"

#ifdef _WIN32
#include "platform/windows/ResourceHelper.h"
#include "platform/windows/ScreenCapture.h"
#else
#include "platform/linux/ResourceHelper.h"
#include "platform/linux/ScreenCapture.h"
#endif

namespace
{
constexpr int kStableOcrFramesBeforeReuse = 3;
constexpr double kOcrPixelDiffThreshold = 8.0;
constexpr double kOcrChangedPixelRatioThreshold = 0.002;
constexpr int kMaxStableOcrIntervalMs = 2000;
constexpr int kOcrSleepChunkMs = 50;

cv::Mat ToOcrGray(const cv::Mat& img)
{
    if (img.channels() == 1)
        return img.clone();

    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

bool IsSimilarOcrFrame(const cv::Mat& currentGray, const cv::Mat& previousGray)
{
    if (currentGray.empty() ||
        previousGray.empty() ||
        currentGray.size() != previousGray.size() ||
        currentGray.type() != previousGray.type())
    {
        return false;
    }

    cv::Mat diff;
    cv::Mat changed;
    cv::absdiff(currentGray, previousGray, diff);
    cv::threshold(diff, changed, kOcrPixelDiffThreshold, 255, cv::THRESH_BINARY);

    const double changedPixels = static_cast<double>(cv::countNonZero(changed));
    const double totalPixels = static_cast<double>(currentGray.total());
    return totalPixels > 0.0 && (changedPixels / totalPixels) < kOcrChangedPixelRatioThreshold;
}

bool HasCloseOverlayText(const std::vector<OverlayText>& texts, int y, int minDistance)
{
    for (const auto& t : texts)
    {
        if (std::abs(t.y - y) < minDistance)
            return true;
    }

    return false;
}

void SleepOcrLoop(std::atomic<bool>& running, const std::atomic<bool>& singleSnapshotRequested, int sleepMs)
{
    int remainingMs = sleepMs;
    while (running && remainingMs > 0 && !singleSnapshotRequested.load())
    {
        const int chunkMs = std::min(remainingMs, kOcrSleepChunkMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(chunkMs));
        remainingMs -= chunkMs;
    }
}
}

OcrService::~OcrService()
{
    Stop();
}

void OcrService::Start(ConfigManager& configManager)
{
    configManager_ = &configManager;
    running_ = true;
    ocrReady_ = false;
    ocrFailed_ = false;
    ocrInitializing_ = true;
    singleSnapshotRequested_ = false;

    AppConfig config;
    {
        std::lock_guard lock(configManager.Mutex());
        config = configManager.Get();
    }

    priceCache_.SetRefreshMinutes(config.priceRefreshMinutes);
    priceCache_.SetLeague(config.priceLeague);
    priceCache_.RefreshIfNeeded();

    initThread_ = std::jthread(
        [this]
        {
            InitOcr();
        });

    workerThread_ = std::jthread(
        [this]
        {
            WorkerLoop();
        });
}

void OcrService::Stop()
{
    running_ = false;

    if (initThread_.joinable())
        initThread_.join();

    if (workerThread_.joinable())
        workerThread_.join();

#ifdef _WIN32
    screenCapture_.Shutdown();
#endif
}

void OcrService::RequestSingleSnapshot()
{
    singleSnapshotRequested_ = true;
}

void OcrService::ForceRefreshPrices()
{
    priceCache_.ForceRefreshAsync();
}

OcrServiceStatus OcrService::GetStatus() const
{
    return {
        ocrInitializing_.load(),
        ocrReady_.load(),
        ocrFailed_.load()
    };
}

PriceServiceStatus OcrService::GetPriceStatus() const
{
    return {
        priceCache_.IsRefreshInProgress(),
        priceCache_.GetPriceCount()
    };
}

DebugData OcrService::GetDebugData()
{
    std::lock_guard lock(debugMutex_);
    return debugData_;
}

bool OcrService::ConsumeOverlayTexts(std::vector<OverlayText>& texts)
{
    if (!overlayDirty_.exchange(false))
        return false;

    std::lock_guard lock(overlayMutex_);
    texts = sharedTexts_;
    return true;
}

void OcrService::InitOcr()
{
    ocrInitializing_ = true;

    LOG_INFO("Initializing OCR");

    std::string tessdata = PrepareTessdata();

    if (!ocr_.Init(tessdata))
    {
        LOG_ERROR("Tesseract init failed");

        ocrFailed_ = true;
        ocrInitializing_ = false;

        return;
    }

    ocrReady_ = true;
    ocrInitializing_ = false;

    LOG_INFO("OCR ready");
}

void OcrService::WorkerLoop()
{
    while (running_ && !ocrReady_)
    {
        if (ocrFailed_)
            return;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    {
        std::lock_guard lock(cachedNamesMutex_);
        cachedItemNames_ = BuildCachedItemNames(priceCache_.GetAllItemNames());
    }

    auto lastRefreshCheck = std::chrono::steady_clock::now();
    cv::Mat lastOcrGray;
    std::vector<LootLine> lastLoot;
    int stableOcrFrames = 0;
    bool forceOcrFrame = false;

    while (running_)
    {
        if (!configManager_)
            return;

        AppConfig localConfig;
        {
            std::lock_guard lock(configManager_->Mutex());
            localConfig = configManager_->Get();
        }

        priceCache_.SetRefreshMinutes(localConfig.priceRefreshMinutes);
        priceCache_.SetLeague(localConfig.priceLeague);

        if (std::chrono::steady_clock::now() - lastRefreshCheck > std::chrono::seconds(10))
        {
            lastRefreshCheck = std::chrono::steady_clock::now();
            priceCache_.RefreshIfNeeded();
            {
                std::lock_guard lock(cachedNamesMutex_);
                cachedItemNames_ = BuildCachedItemNames(priceCache_.GetAllItemNames());
            }
        }

        bool runSingleSnapshot = singleSnapshotRequested_.exchange(false);

        if (runSingleSnapshot)
            singleSnapshotUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        bool keepSnapshot = std::chrono::steady_clock::now() < singleSnapshotUntil_;

        if (!localConfig.ocrEnabled && !runSingleSnapshot && !keepSnapshot)
        {
            lastOcrGray.release();
            lastLoot.clear();
            stableOcrFrames = 0;
            ClearOverlayTexts();
            SleepOcrLoop(running_, singleSnapshotRequested_, 100);
            continue;
        }

        if (localConfig.regionW <= 0 || localConfig.regionH <= 0)
        {
            lastOcrGray.release();
            lastLoot.clear();
            stableOcrFrames = 0;
            SleepOcrLoop(running_, singleSnapshotRequested_, 100);
            continue;
        }

        cv::Rect localRegion(
            localConfig.regionX,
            localConfig.regionY,
            localConfig.regionW,
            localConfig.regionH
        );

#ifdef _WIN32
        cv::Mat img = screenCapture_.CaptureRegion(localRegion);

        if (img.empty())
            img = CaptureRegion(localRegion);
#else
        cv::Mat img = CaptureRegion(localRegion);
#endif

        if (!img.empty())
        {
            std::vector<LootLine> loot;
            cv::Mat currentGray = ToOcrGray(img);
            const bool similarFrame = !forceOcrFrame && IsSimilarOcrFrame(currentGray, lastOcrGray);

            if (similarFrame)
                ++stableOcrFrames;
            else
                stableOcrFrames = 0;

            if (similarFrame && stableOcrFrames >= kStableOcrFramesBeforeReuse && !lastLoot.empty())
            {
                loot = lastLoot;
            }
            else
            {
                loot = ocr_.RecognizeLoot(img, localConfig);
                lastLoot = loot;
                forceOcrFrame = false;
            }

            lastOcrGray = std::move(currentGray);

            DebugData debug;
            std::vector<OverlayText> newTexts;

            std::vector<CachedItemName> cachedNames;
            {
                std::lock_guard lock(cachedNamesMutex_);
                cachedNames = cachedItemNames_;
            }

            for (const auto& item : loot)
            {
                DebugLine debugLine;
                debugLine.ocrText = item.text;
                debugLine.matchedText = "-";
                debugLine.price = "-";
                debugLine.confidence = 0;

                auto parsed = LootParser::ParseLootLine(item.text);

                std::string rawName = parsed.itemName;
                int quantity = parsed.quantity;

                auto price = priceCache_.GetPrice(rawName);

                if (price)
                {
                    debugLine.matchedText = rawName;
                    debugLine.confidence = 100;
                }
                else
                {
                    auto guess = FindBestItemMatch(rawName, cachedNames);

                    if (guess)
                    {
                        debugLine.matchedText = guess->name;
                        debugLine.confidence = guess->confidence;
                        price = priceCache_.GetPrice(guess->name);
                    }
                }

                if (!price)
                {
                    debug.lines.push_back(std::move(debugLine));
                    continue;
                }

                debugLine.price = *price;

                std::optional<double> value = LootParser::ParsePriceValue(*price);
                double totalValue = value ? (*value * quantity) : 0.0;

                int overlayY = localRegion.y + (item.y1 + item.y2) / 2 + localConfig.overlayOffsetY;

                if (HasCloseOverlayText(newTexts, overlayY, 25))
                {
                    debug.lines.push_back(std::move(debugLine));
                    continue;
                }

                OverlayText t;
                t.color = GetPriceColor(totalValue, localConfig);
                t.text = ToWide(LootParser::FormatStackPrice(*price, quantity));
                t.x = localRegion.x + localRegion.width + localConfig.overlayOffsetX;
                t.y = localRegion.y + (item.y1 + item.y2) / 2 + localConfig.overlayOffsetY;

                newTexts.push_back(std::move(t));

                debug.lines.push_back(std::move(debugLine));
            }

            SetOverlayTexts(std::move(newTexts));

            {
                std::lock_guard lock(debugMutex_);
                debugData_ = std::move(debug);
            }
        }

        int sleepMs = localConfig.ocrIntervalMs;
        if (stableOcrFrames >= kStableOcrFramesBeforeReuse)
            sleepMs = std::min(localConfig.ocrIntervalMs * 2, kMaxStableOcrIntervalMs);

        SleepOcrLoop(running_, singleSnapshotRequested_, sleepMs);
    }
}

void OcrService::ClearOverlayTexts()
{
    SetOverlayTexts({});
}

void OcrService::SetOverlayTexts(std::vector<OverlayText> texts)
{
    std::lock_guard lock(overlayMutex_);
    sharedTexts_ = std::move(texts);
    overlayDirty_ = true;
}
