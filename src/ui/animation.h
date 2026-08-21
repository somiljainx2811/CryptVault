// animation.h - small, dependency-free time/easing helpers so
// animated UI state ("this card's hover-glow amount", "this tab
// indicator's slide position") is driven by wall-clock time read
// from UiAppInput::now_seconds, rather than by hand-incrementing a
// float once per frame (e.g. `x += 1.0f;`) - the latter ties the
// animation's speed to the frame rate, which breaks the moment
// vsync/refresh rate changes.
//
// Everything here is a pure function of (now, start, duration) or of
// a 0..1 progress value, plus one small piece of state (Timeline)
// for the common "remember when this started" case. Nothing here
// touches UiRenderer/ContentRenderer - callers compute a 0..1 value
// with this header, then feed it into DrawRect/DrawRoundedRect/etc.
// calls themselves (e.g. Lerp a color's alpha, or a rect's position).

#ifndef APPSHELL_UI_ANIMATION_H
#define APPSHELL_UI_ANIMATION_H

#include <cmath>

namespace appshell {
namespace anim {

// --- suggested durations, by category (see the animation-categories
// note this mirrors: micro/UI/major) ---------------------------------
//
// These are starting points, not hard rules - pass any float duration
// to AnimationProgress/Timeline. Grouping them here just keeps new
// call sites consistent instead of every feature picking its own
// one-off number.
namespace duration {
constexpr float kMicro = 0.10f;  // hover, button press, icon brightness, selection
constexpr float kUi = 0.25f;     // panels, tabs, menus, dialogs, search, notifications
constexpr float kMajor = 0.60f;  // vault opening, encryption, unlock, screen transitions
}  // namespace duration

inline float Clamp01(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

inline float Lerp(float from, float to, float t) {
    return from + (to - from) * t;
}

// Linear 0..1 progress of a `duration_seconds`-long span that began
// at `start_seconds`, measured against `now_seconds` (all three in
// the same clock - see UiAppInput::now_seconds). A non-positive
// duration is treated as "already finished" (returns 1) rather than
// dividing by zero or returning NaN/inf.
inline float AnimationProgress(double now_seconds, double start_seconds, float duration_seconds) {
    if (duration_seconds <= 0.0f) {
        return 1.0f;
    }
    return Clamp01(static_cast<float>((now_seconds - start_seconds) / duration_seconds));
}

// --- easing curves -----------------------------------------------------
// Each takes/returns a 0..1 value (inputs outside that range are
// clamped first). These are the standard formulas, not approximations
// built from lookup tables, so they stay correct at any duration.

inline float EaseLinear(float t) {
    return Clamp01(t);
}

inline float EaseInCubic(float t) {
    t = Clamp01(t);
    return t * t * t;
}

inline float EaseOutCubic(float t) {
    t = Clamp01(t);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

inline float EaseInOutCubic(float t) {
    t = Clamp01(t);
    if (t < 0.5f) {
        return 4.0f * t * t * t;
    }
    float inv = -2.0f * t + 2.0f;
    return 1.0f - (inv * inv * inv) * 0.5f;
}

// Overshoots past 1.0 before settling back - good for "pop in" menus/
// dialogs/cards (see the 0.96 -> 1.0 scale-in animations in the
// design notes). Standard easings.net formula; c1/c3 control how far
// past 1.0 it overshoots.
inline float EaseOutBack(float t) {
    t = Clamp01(t);
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    float inv = t - 1.0f;
    return 1.0f + c3 * inv * inv * inv + c1 * inv * inv;
}

// Damped-oscillation "spring" settle: overshoots and wobbles briefly
// before landing on 1.0. Standard easings.net elastic-out formula.
// Good for the unlock/vault-opening "important" moments the design
// notes call out, used sparingly since it draws the eye.
inline float EaseOutSpring(float t) {
    t = Clamp01(t);
    if (t <= 0.0f || t >= 1.0f) {
        return t;
    }
    constexpr float kTwoPi = 6.28318530718f;
    constexpr float c4 = kTwoPi / 3.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

enum class Easing {
    kLinear,
    kEaseInCubic,
    kEaseOutCubic,
    kEaseInOutCubic,
    kEaseOutBack,
    kEaseOutSpring,
};

// Single dispatch point so call sites can store an Easing enum value
// (e.g. per-widget, or in a table) instead of a function pointer.
inline float Ease(Easing easing, float t) {
    switch (easing) {
        case Easing::kLinear: return EaseLinear(t);
        case Easing::kEaseInCubic: return EaseInCubic(t);
        case Easing::kEaseOutCubic: return EaseOutCubic(t);
        case Easing::kEaseInOutCubic: return EaseInOutCubic(t);
        case Easing::kEaseOutBack: return EaseOutBack(t);
        case Easing::kEaseOutSpring: return EaseOutSpring(t);
    }
    return EaseLinear(t);
}

// Moves `current` toward `target` at a constant rate of
// (1 / duration_seconds) per second, clamped so it never overshoots.
// Timeline (above) is one-shot and start/duration based - great for
// "this began at time X" events (a panel opening), but a poor fit for
// continuous, reversible values like "how hovered is this tile right
// now", where the target can flip direction on any frame (mouse
// enters, then leaves before the animation finished). MoveTowards
// handles that case: call it every frame with the current value and
// today's target, in place of hand-rolling `x += 1.0f`-style
// per-frame increments.
inline float MoveTowards(float current, float target, float dt_seconds, float duration_seconds) {
    if (duration_seconds <= 0.0f) {
        return target;
    }
    float max_delta = dt_seconds / duration_seconds;
    if (current < target) {
        return current + max_delta < target ? current + max_delta : target;
    }
    if (current > target) {
        return current - max_delta > target ? current - max_delta : target;
    }
    return current;
}

// --- Timeline: the one piece of actual state ----------------------------
//
// Wraps "remember when this animation started" so call sites don't
// each reinvent it. Typical use, stored as a field on whatever
// persistent per-widget state already lives in AppState (e.g. one
// Timeline per vault tile for its hover-glow fade):
//
//   if (hovered && !tile.hover_timeline.started()) {
//       tile.hover_timeline.Start(state.now_seconds);
//   } else if (!hovered) {
//       tile.hover_timeline.Reset();
//   }
//   float glow = anim::Ease(anim::Easing::kEaseOutCubic,
//                            tile.hover_timeline.Progress(state.now_seconds));
//
// Deliberately doesn't own an easing function or apply one itself -
// keeping "when did this start" separate from "what curve does it
// follow" means the same Timeline can drive different curves for
// different properties of the same animation (e.g. position eases
// one way, opacity another).
struct Timeline {
    double start_seconds = -1.0;
    float duration_seconds = duration::kUi;

    void Start(double now_seconds) { start_seconds = now_seconds; }
    void Reset() { start_seconds = -1.0; }
    bool started() const { return start_seconds >= 0.0; }

    // 0 before Start() has been called; otherwise linear 0..1
    // progress through duration_seconds (see AnimationProgress).
    float Progress(double now_seconds) const {
        if (!started()) return 0.0f;
        return AnimationProgress(now_seconds, start_seconds, duration_seconds);
    }

    bool Finished(double now_seconds) const {
        return started() && Progress(now_seconds) >= 1.0f;
    }
};

}  // namespace anim
}  // namespace appshell

#endif  // APPSHELL_UI_ANIMATION_H
