#pragma once

#include <opencv2/core.hpp>

#include <vector>

// Finding a remembered object in a screen frame. Pure OpenCV, no Qt, so it can be tested.
namespace matcher
{
constexpr double kLostScore = 0.5;       // loose: any close look-alike will do
constexpr double kRelocateScore = 0.75;  // strict: only jump to the chosen object when sure
constexpr double kReturnScore = 0.6;     // going back to the chosen object from a substitute
constexpr double kRelocateMargin = 0.1;  // must beat the current box by this much
constexpr double kBigChangeDiff = 20.0;  // mean gray difference (0-255) between frames

// True when the whole picture differs strongly from the previous frame.
// Stores the current frame as the new reference.
bool frameChanged(const cv::Mat &frame, cv::Mat &previous);

// What the user chose, as a few views of it. The first view is the original and is
// never replaced; later views (other lighting, angle) only help to recognise it.
class TargetMemory
{
public:
    static constexpr size_t kMaxViews = 6;

    void setChosen(const cv::Mat &crop);
    void addView(const cv::Mat &crop);
    bool empty() const { return m_views.empty(); }
    size_t viewCount() const { return m_views.size(); }
    const cv::Mat &chosen() const { return m_views.front(); }

    // Best correlation of any view anywhere in `frame`; `found` is its box. With `autoSize`
    // each view is also tried at 80%-120% of its size. Searches half-size first, then refines.
    double best(const cv::Mat &frame, cv::Rect &found, bool autoSize) const;
    // Best correlation of any view with the frame content under `box`.
    double scoreAt(const cv::Mat &frame, const cv::Rect &box) const;

private:
    std::vector<cv::Mat> m_views;
};

enum class Outcome {
    None,       // nothing similar, keep looking
    Chosen,     // the chosen object itself
    Substitute, // only a look-alike
};
// What a score found while the target is lost means.
Outcome afterLoss(double score);
// Whether to jump to `best` (the chosen object found elsewhere) from the current box.
// `far`: it is away from the current box. `scoreHere`: how well the current box matches the chosen object.
bool switchTarget(bool onSubstitute, double best, bool far, double scoreHere);
}
