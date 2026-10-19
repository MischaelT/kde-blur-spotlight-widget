#include "matcher.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace matcher
{
namespace
{
// Edge strength of an image. Unlike raw colors it survives exposure and contrast changes.
cv::Mat gradient(const cv::Mat &image)
{
    cv::Mat gray, dx, dy, magnitude;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0);
    cv::Sobel(gray, dx, CV_32F, 1, 0);
    cv::Sobel(gray, dy, CV_32F, 0, 1);
    cv::magnitude(dx, dy, magnitude);
    return magnitude;
}

// Correlation of `view` inside `frame(roi)`, by color and by edges (the better counts, so a
// changed exposure still matches). `frameEdges` is gradient(frame) when searching the whole frame.
double correlate(const cv::Mat &frame, const cv::Mat &frameEdges, const cv::Mat &view, const cv::Rect &roi,
                 cv::Rect &found)
{
    if (view.cols < 4 || view.rows < 4 || roi.width < view.cols || roi.height < view.rows) {
        return -1.0;
    }
    const cv::Mat viewEdges = gradient(view);
    double best = -1.0;
    for (int pass = 0; pass < 2; ++pass) {
        cv::Mat result;
        if (pass == 0) {
            cv::matchTemplate(frame(roi), view, result, cv::TM_CCOEFF_NORMED);
        } else {
            const cv::Mat edges = frameEdges.empty() ? gradient(frame(roi)) : frameEdges(roi);
            cv::matchTemplate(edges, viewEdges, result, cv::TM_CCOEFF_NORMED);
        }
        double score;
        cv::Point where;
        cv::minMaxLoc(result, nullptr, &score, nullptr, &where);
        if (std::isfinite(score) && score > best) {
            best = score;
            found = cv::Rect(roi.tl() + where, view.size());
        }
    }
    return best;
}

constexpr int kRefinePad = 6; // full-size pixels searched around a coarse hit
}

bool frameChanged(const cv::Mat &frame, cv::Mat &previous)
{
    cv::Mat gray;
    cv::resize(frame, gray, cv::Size(64, 36), 0, 0, cv::INTER_AREA);
    cv::cvtColor(gray, gray, cv::COLOR_BGR2GRAY);
    const bool changed = !previous.empty() && cv::mean(cv::abs(gray - previous))[0] > kBigChangeDiff;
    previous = gray;
    return changed;
}

void TargetMemory::setChosen(const cv::Mat &crop)
{
    m_views.clear();
    m_views.push_back(crop.clone());
}

void TargetMemory::addView(const cv::Mat &crop)
{
    if (m_views.empty()) {
        return;
    }
    if (m_views.size() >= kMaxViews) {
        m_views.erase(m_views.begin() + 1); // drop the oldest extra view, never the chosen one
    }
    m_views.push_back(crop.clone());
}

double TargetMemory::best(const cv::Mat &frame, cv::Rect &found, bool autoSize) const
{
    if (m_views.empty() || frame.empty()) {
        return -1.0;
    }
    static const double kScales[] = {1.0, 0.8, 0.9, 1.1, 1.2};
    const cv::Rect whole(0, 0, frame.cols, frame.rows);

    cv::Mat half;
    cv::resize(frame, half, cv::Size(), 0.5, 0.5, cv::INTER_AREA);
    const cv::Mat halfEdges = gradient(half);

    double best = -1.0;
    for (const cv::Mat &view : m_views) {
        for (const double factor : kScales) {
            if (factor != 1.0 && !autoSize) {
                break;
            }
            cv::Mat sized;
            if (factor == 1.0) {
                sized = view;
            } else {
                cv::resize(view, sized, cv::Size(), factor, factor, factor < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
            }
            if (sized.cols < 8 || sized.rows < 8 || sized.cols > frame.cols || sized.rows > frame.rows) {
                continue;
            }

            cv::Rect hit;
            cv::Mat halfView;
            cv::resize(sized, halfView, cv::Size(), 0.5, 0.5, cv::INTER_AREA);
            const cv::Rect halfWhole(0, 0, half.cols, half.rows);
            const double coarse = correlate(half, halfEdges, halfView, halfWhole, hit);
            if (coarse < 0.0) {
                continue;
            }
            // Refine at full size around the coarse hit.
            const cv::Rect around(hit.x * 2 - kRefinePad, hit.y * 2 - kRefinePad,
                                  sized.cols + 2 * kRefinePad, sized.rows + 2 * kRefinePad);
            cv::Rect refined;
            const double score = correlate(frame, cv::Mat(), sized, around & whole, refined);
            if (score > best) {
                best = score;
                found = refined;
            }
        }
    }
    return best;
}

double TargetMemory::scoreAt(const cv::Mat &frame, const cv::Rect &box) const
{
    const cv::Rect inside = box & cv::Rect(0, 0, frame.cols, frame.rows);
    if (m_views.empty() || inside.width < 2 || inside.height < 2) {
        return -1.0;
    }
    double best = -1.0;
    for (const cv::Mat &view : m_views) {
        cv::Mat patch;
        cv::resize(frame(inside), patch, view.size());
        cv::Rect ignored;
        best = std::max(best, correlate(patch, cv::Mat(), view, cv::Rect(0, 0, patch.cols, patch.rows), ignored));
    }
    return best;
}

Outcome afterLoss(double score)
{
    if (score >= kRelocateScore) {
        return Outcome::Chosen;
    }
    return score >= kLostScore ? Outcome::Substitute : Outcome::None;
}

bool switchTarget(bool onSubstitute, double best, bool far, double scoreHere)
{
    if (onSubstitute) {
        return best >= kReturnScore && (far || scoreHere < kReturnScore);
    }
    return best >= kRelocateScore && far && best > scoreHere + kRelocateMargin;
}
}
