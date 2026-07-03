#pragma once

#include <opencv2/core.hpp>

class OcrFrameDiffer
{
public:
    bool IsSimilarFrame(const cv::Mat& img, bool forceFrame);
    void StoreFrame(cv::Mat gray);
    void Reset();

    int StableFrames() const;

private:
    static cv::Mat ToGray(const cv::Mat& img);

private:
    cv::Mat lastGray_;
    int stableFrames_ = 0;
};
