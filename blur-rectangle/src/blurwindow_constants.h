#pragma once

// Sizes and timings shared by the BlurWindow source files.
namespace blurwindow
{
constexpr int kMinimumWidth = 160;
constexpr int kMinimumHeight = 56;
constexpr int kResizeMargin = 10;
constexpr int kVisualRadius = 5;
constexpr int kTitleButtonWidth = 46;
constexpr int kTitleBarHeight = 30;
constexpr int kSpotlightResizeMargin = 10;
constexpr int kMinimumSpotlightRadius = 48;
constexpr int kMaximumSpotlightRadius = 520;
constexpr int kMinimumSpotlightRectWidth = 96;
constexpr int kMinimumSpotlightRectHeight = 64;
constexpr int kMaxSpotlights = 3;
constexpr int kTrackDeadZone = 2;
constexpr double kTrackSmoothTime = 0.1; // seconds to settle onto a new target
constexpr int kTrackFrameMs = 16;
constexpr int kAutoTrackIdleMs = 3000;
constexpr int kAutoTrackSettleMs = 200;   // standstill after which the object under the spotlight is memorized
constexpr int kFollowMargin = 160;       // extra input area around a following spotlight while the pointer moves
constexpr int kFollowSettleMs = 250;
constexpr double kAutoSizeRange = 0.2;
constexpr double kZoomFactors[] = {1.0, 1.15, 1.25, 1.5, 2.0, 3.0};
}
