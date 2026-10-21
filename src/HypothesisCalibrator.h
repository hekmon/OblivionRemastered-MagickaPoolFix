// HypothesisCalibrator.h - MagickaPoolFix
//
// Determine empirically, at runtime, what
// PlayerCharacter::GetActorFloatValue(ActorValue::Magicka) actually returns:
//   hypothesis A: CURRENT magicka  -> max     = v - modifier
//   hypothesis B: MAXIMUM magicka  -> current = v + modifier
//
// The discriminator
// -----------------
// The maximum is piecewise constant: it only moves on a Fortify, a Drain or a
// level-up. The modifier moves constantly: every cast lowers it, natural
// regeneration raises it.
//
// So keep two candidates for the maximum:
//     candA = v - modifier      (correct if hypothesis A holds)
//     candB = v                 (correct if hypothesis B holds)
//
// Every time the modifier changes WITHOUT the real maximum changing, the correct
// candidate stays constant while the other moves by exactly -delta. The candidate
// that stays stable across modifier variations identifies the right hypothesis.
//
// Properties of the method:
//   - no player action required: natural regeneration alone produces calibration
//     events within a fraction of a second;
//   - no knowledge of spell costs or of the expected pool value;
//   - well-posed: if the modifier changed, both candidates cannot be stable at the
//     same time, since candA = v - m means a stable v forces candA to move by
//     -delta and vice versa. An event where NEITHER is stable means v and the
//     maximum moved on the same frame: the event is ambiguous and is not counted.
//
// Self-contained header: no Windows dependency, testable outside the game.

#pragma once

#include <cmath>
#include <cstdint>

namespace mpf {

enum class Hypothesis : std::int32_t {
    Unknown        = 0,  // calibration in progress, or inconclusive
    ValueIsCurrent = 1,  // hypothesis A
    ValueIsMax     = 2,  // hypothesis B
};

struct CalibrationConfig {
    // Number of unambiguous events required before concluding.
    int   requiredEvents = 8;
    // Minimum gap between the two scores for the verdict to count as decisive.
    int   decisiveMargin = 4;
    // Float comparison tolerance.
    float epsilon = 0.01f;
    // Beyond this, give up on automatic calibration (0 = never give up).
    // 3600 frames is about 60 s at 60 fps.
    int   maxFrames = 3600;
};

// Outcome of ingesting one frame.
struct CalibrationStep {
    bool       eventObserved = false;  // the modifier changed on this frame
    bool       eventCounted  = false;  // ... and the event was unambiguous
    bool       justResolved  = false;  // the verdict was just reached
    bool       justGaveUp    = false;  // maxFrames hit without a verdict
    Hypothesis hypothesis    = Hypothesis::Unknown;
    // Per-event detail, so the caller can log both hypotheses side by side at
    // Debug level without recomputing anything.
    float      candA         = 0.0f;
    float      candB         = 0.0f;
    bool       stableA       = false;
    bool       stableB       = false;
};

class Calibrator {
public:
    explicit Calibrator(const CalibrationConfig& cfg = {}) : cfg_(cfg) {}

    // Call once per frame, before any correction.
    //   v = GetActorFloatValue(Magicka)
    //   m = *(float*)(player + offsetMagickaModifier)
    CalibrationStep Feed(float v, float m) {
        CalibrationStep step;
        step.hypothesis = hypothesis_;

        if (hypothesis_ != Hypothesis::Unknown || gaveUp_) {
            return step;  // already settled: zero cost
        }

        if (!std::isfinite(v) || !std::isfinite(m)) {
            return step;  // unusable frame, ignored without consequence
        }

        ++frames_;

        const float candA = v - m;  // the maximum, if v is current magicka
        const float candB = v;      // the maximum, if v is maximum magicka
        step.candA = candA;
        step.candB = candB;

        if (!primed_) {
            primed_    = true;
            prevM_     = m;
            prevCandA_ = candA;
            prevCandB_ = candB;
            return step;
        }

        const float dm = m - prevM_;

        if (std::fabs(dm) > cfg_.epsilon) {
            step.eventObserved = true;

            const bool stableA = std::fabs(candA - prevCandA_) <= cfg_.epsilon;
            const bool stableB = std::fabs(candB - prevCandB_) <= cfg_.epsilon;
            step.stableA = stableA;
            step.stableB = stableB;

            // Exactly one of the two must be stable. Otherwise the maximum moved at
            // the same time as the modifier: ambiguous event, not counted.
            if (stableA != stableB) {
                step.eventCounted = true;
                if (stableA) { ++scoreA_; } else { ++scoreB_; }
                ++events_;
            }
        }

        prevM_     = m;
        prevCandA_ = candA;
        prevCandB_ = candB;

        if (events_ >= cfg_.requiredEvents) {
            const int diff = scoreA_ - scoreB_;
            if (diff >= cfg_.decisiveMargin) {
                hypothesis_       = Hypothesis::ValueIsCurrent;
                step.justResolved = true;
            } else if (-diff >= cfg_.decisiveMargin) {
                hypothesis_       = Hypothesis::ValueIsMax;
                step.justResolved = true;
            }
            // Otherwise keep accumulating: the verdict is not decisive yet.
        }

        if (hypothesis_ == Hypothesis::Unknown && cfg_.maxFrames > 0 &&
            frames_ >= cfg_.maxFrames) {
            gaveUp_         = true;
            step.justGaveUp = true;
        }

        step.hypothesis = hypothesis_;
        return step;
    }

    // Force the verdict ([Offsets] iForceHypothesis, the manual escape hatch).
    void Force(Hypothesis h) { hypothesis_ = h; gaveUp_ = false; }

    Hypothesis Get()      const { return hypothesis_; }
    bool       Resolved() const { return hypothesis_ != Hypothesis::Unknown; }
    bool       GaveUp()   const { return gaveUp_; }
    int        ScoreA()   const { return scoreA_; }
    int        ScoreB()   const { return scoreB_; }
    int        Events()   const { return events_; }
    int        Frames()   const { return frames_; }

    // Read the (current, max) pair according to the accepted hypothesis.
    // Returns false when calibration has not concluded: in that case the caller
    // MUST NOT perform any memory write.
    bool Resolve(float v, float m, float& outCurrent, float& outMax) const {
        switch (hypothesis_) {
        case Hypothesis::ValueIsCurrent:
            outCurrent = v;
            outMax     = v - m;
            return true;
        case Hypothesis::ValueIsMax:
            outCurrent = v + m;
            outMax     = v;
            return true;
        default:
            return false;
        }
    }

private:
    CalibrationConfig cfg_{};
    Hypothesis hypothesis_ = Hypothesis::Unknown;
    bool  primed_ = false;
    bool  gaveUp_ = false;
    int   frames_ = 0;
    int   events_ = 0;
    int   scoreA_ = 0;
    int   scoreB_ = 0;
    float prevM_     = 0.0f;
    float prevCandA_ = 0.0f;
    float prevCandB_ = 0.0f;
};

inline const char* HypothesisName(Hypothesis h) {
    switch (h) {
    case Hypothesis::ValueIsCurrent: return "GetActorFloatValue = CURRENT magicka";
    case Hypothesis::ValueIsMax:     return "GetActorFloatValue = MAX magicka";
    default:                         return "UNKNOWN (calibration pending)";
    }
}

}  // namespace mpf
