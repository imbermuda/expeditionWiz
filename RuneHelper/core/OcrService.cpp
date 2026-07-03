#include "core/OcrService.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "core/Logger.h"
#include "ocr/LootOverlayBuilder.h"

#ifdef _WIN32
#include "platform/windows/ResourceHelper.h"
#else
#include "platform/linux/ResourceHelper.h"
#endif

namespace
{
constexpr int kStableOcrFramesBeforeReuse = 3;
constexpr int kMaxStableOcrIntervalMs = 2000;
constexpr int kOcrSleepChunkMs = 50;

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

    screenCapture_.Shutdown();
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
    std::vector<LootLine> lastLoot;
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
            frameDiffer_.Reset();
            lastLoot.clear();
            ClearOverlayTexts();
            SleepOcrLoop(running_, singleSnapshotRequested_, 100);
            continue;
        }

        if (localConfig.regionW <= 0 || localConfig.regionH <= 0)
        {
            frameDiffer_.Reset();
            lastLoot.clear();
            SleepOcrLoop(running_, singleSnapshotRequested_, 100);
            continue;
        }

        cv::Rect localRegion(
            localConfig.regionX,
            localConfig.regionY,
            localConfig.regionW,
            localConfig.regionH
        );

        cv::Mat img = screenCapture_.CaptureRegion(localRegion);

        if (!img.empty())
        {
            std::vector<LootLine> loot;
            const bool similarFrame = frameDiffer_.IsSimilarFrame(img, forceOcrFrame);

            if (similarFrame && frameDiffer_.StableFrames() >= kStableOcrFramesBeforeReuse && !lastLoot.empty())
            {
                loot = lastLoot;
            }
            else
            {
                loot = ocr_.RecognizeLoot(img, localConfig);
                lastLoot = loot;
                forceOcrFrame = false;
            }

            frameDiffer_.StoreFrame(img);

            std::vector<CachedItemName> cachedNames;
            {
                std::lock_guard lock(cachedNamesMutex_);
                cachedNames = cachedItemNames_;
            }

            LootOverlayBuildResult buildResult = LootOverlayBuilder::Build(
                loot,
                localRegion,
                localConfig,
                priceCache_,
                cachedNames
            );

            SetOverlayTexts(std::move(buildResult.texts));

            {
                std::lock_guard lock(debugMutex_);
                debugData_ = std::move(buildResult.debug);
            }
        }

        int sleepMs = localConfig.ocrIntervalMs;
        if (frameDiffer_.StableFrames() >= kStableOcrFramesBeforeReuse)
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
