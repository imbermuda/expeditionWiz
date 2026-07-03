#include "ocr/OcrFrameDiffer.h"

#include <utility>

#include <opencv2/imgproc.hpp>

namespace
{
constexpr double kOcrPixelDiffThreshold = 8.0;
constexpr double kOcrChangedPixelRatioThreshold = 0.002;
}

bool OcrFrameDiffer::IsSimilarFrame(const cv::Mat& img, bool forceFrame)
{
    if (forceFrame)
    {
        stableFrames_ = 0;
        return false;
    }

    cv::Mat currentGray = ToGray(img);

    if (currentGray.empty() ||
        lastGray_.empty() ||
        currentGray.size() != lastGray_.size() ||
        currentGray.type() != lastGray_.type())
    {
        stableFrames_ = 0;
        return false;
    }

    cv::Mat diff;
    cv::Mat changed;
    cv::absdiff(currentGray, lastGray_, diff);
    cv::threshold(diff, changed, kOcrPixelDiffThreshold, 255, cv::THRESH_BINARY);

    const double changedPixels = static_cast<double>(cv::countNonZero(changed));
    const double totalPixels = static_cast<double>(currentGray.total());
    const bool similar = totalPixels > 0.0 && (changedPixels / totalPixels) < kOcrChangedPixelRatioThreshold;

    if (similar)
        ++stableFrames_;
    else
        stableFrames_ = 0;

    return similar;
}

void OcrFrameDiffer::StoreFrame(cv::Mat img)
{
    lastGray_ = ToGray(img);
}

void OcrFrameDiffer::Reset()
{
    lastGray_.release();
    stableFrames_ = 0;
}

int OcrFrameDiffer::StableFrames() const
{
    return stableFrames_;
}

cv::Mat OcrFrameDiffer::ToGray(const cv::Mat& img)
{
    if (img.empty())
        return {};

    if (img.channels() == 1)
        return img.clone();

    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    return gray;
}
