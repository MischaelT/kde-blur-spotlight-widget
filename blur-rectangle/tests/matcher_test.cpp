// Tests for the object matching in src/matcher.cpp, on synthetic frames.
#include "matcher.h"

#include <opencv2/imgproc.hpp>

#include <cstdio>
#include <cstdlib>

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

cv::Mat texture(cv::Size size, int seed, double contrast)
{
    cv::RNG rng(seed);
    cv::Mat noise(size, CV_8UC3);
    rng.fill(noise, cv::RNG::UNIFORM, 0, 256);
    cv::GaussianBlur(noise, noise, cv::Size(0, 0), 2.0);
    cv::Mat result;
    noise.convertTo(result, CV_8UC3, contrast, 128 * (1 - contrast));
    return result;
}

cv::Mat background()
{
    return texture(cv::Size(640, 360), 1, 0.5);
}

cv::Mat object()
{
    return texture(cv::Size(64, 64), 2, 1.0);
}

// A frame with the object pasted at `at`, optionally scaled.
cv::Mat frameWith(const cv::Mat &obj, cv::Point at, double scale = 1.0)
{
    cv::Mat frame = background().clone();
    cv::Mat sized;
    cv::resize(obj, sized, cv::Size(), scale, scale, cv::INTER_LINEAR);
    sized.copyTo(frame(cv::Rect(at, sized.size())));
    return frame;
}

double distance(const cv::Rect &a, cv::Point p)
{
    return std::hypot(a.x - p.x, a.y - p.y);
}
}

int main()
{
    using namespace matcher;
    const cv::Mat obj = object();
    TargetMemory memory;
    memory.setChosen(obj);

    cv::Rect found;
    double score = memory.best(frameWith(obj, {200, 100}), found, false);
    check(score > 0.9 && distance(found, {200, 100}) <= 2, "finds the object where it is");

    score = memory.best(frameWith(obj, {410, 233}), found, false);
    check(score > 0.9 && distance(found, {410, 233}) <= 2, "finds it after it moved");

    cv::Mat darker;
    frameWith(obj, {300, 150}).convertTo(darker, -1, 0.6, -10);
    score = memory.best(darker, found, false);
    check(score >= kRelocateScore && distance(found, {300, 150}) <= 2, "finds it after an exposure drop");

    cv::Mat brighter;
    frameWith(obj, {300, 150}).convertTo(brighter, -1, 1.3, 30);
    score = memory.best(brighter, found, false);
    check(score >= kRelocateScore && distance(found, {300, 150}) <= 2, "finds it after an exposure rise");

    score = memory.best(background(), found, false);
    check(score < kRelocateScore, "does not claim the object when it is absent");

    const cv::Mat bigger = frameWith(obj, {250, 120}, 1.15);
    const double fixedScore = memory.best(bigger, found, false);
    const double autoScore = memory.best(bigger, found, true);
    check(autoScore > fixedScore && autoScore >= kRelocateScore && found.width > obj.cols, "autosize finds a 15% bigger object");

    const cv::Mat smaller = frameWith(obj, {250, 120}, 0.85);
    check(memory.best(smaller, found, true) >= kRelocateScore && found.width < obj.cols, "autosize finds a 15% smaller object");

    // The chosen view is never replaced, extra views are capped.
    TargetMemory views;
    views.setChosen(obj);
    for (int i = 0; i < 10; ++i) {
        views.addView(texture(cv::Size(64, 64), 100 + i, 1.0));
    }
    check(views.viewCount() == TargetMemory::kMaxViews, "views are capped");
    check(cv::norm(views.chosen(), obj, cv::NORM_INF) == 0, "the chosen view is never replaced");

    // An extra view helps to recognise a changed look of the object.
    cv::Mat changedObj = obj.clone();
    cv::rectangle(changedObj, cv::Rect(10, 10, 30, 30), cv::Scalar(255, 255, 255), cv::FILLED);
    const cv::Mat changedFrame = frameWith(changedObj, {180, 200});
    TargetMemory plain;
    plain.setChosen(obj);
    TargetMemory learned;
    learned.setChosen(obj);
    learned.addView(changedObj);
    check(learned.best(changedFrame, found, false) > plain.best(changedFrame, found, false), "a saved view recognises a changed look better");
    check(learned.best(changedFrame, found, false) > 0.9 && distance(found, {180, 200}) <= 2, "... and finds it");

    check(memory.scoreAt(frameWith(obj, {200, 100}), cv::Rect(200, 100, 64, 64)) > 0.9, "scoreAt is high on the object");
    check(memory.scoreAt(frameWith(obj, {200, 100}), cv::Rect(400, 250, 64, 64)) < 0.5, "scoreAt is low off the object");

    // Decisions.
    check(afterLoss(0.9) == Outcome::Chosen, "a strong match after a loss is the chosen object");
    check(afterLoss(0.6) == Outcome::Substitute, "a loose match after a loss is a substitute");
    check(afterLoss(0.3) == Outcome::None, "no match after a loss keeps searching");
    check(switchTarget(true, 0.65, true, 0.2), "from a substitute, go back to the chosen object when found");
    check(!switchTarget(true, 0.4, true, 0.2), "from a substitute, stay when the chosen object is not found");
    check(!switchTarget(false, 0.8, true, 0.78), "on the chosen object, do not jump for a marginal win");
    check(switchTarget(false, 0.9, true, 0.2), "on the chosen object, jump when clearly elsewhere");
    check(!switchTarget(false, 0.9, false, 0.2), "on the chosen object, do not jump to where it already is");

    // Whole-picture change detection.
    cv::Mat previous;
    const cv::Mat base = frameWith(obj, {100, 100});
    check(!frameChanged(base, previous), "first frame is not a change");
    check(!frameChanged(base, previous), "same frame is not a change");
    cv::Mat shifted;
    base.convertTo(shifted, -1, 1.0, 70);
    check(frameChanged(shifted, previous), "a big exposure jump is a change");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
