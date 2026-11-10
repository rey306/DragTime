// DragTime engine: tempo-synced slow-playback buffer effect.
// Plain C++17, no JUCE in this file, so the DSP can be edited and tested alone.
//
// How it works
//   1. The input is continuously recorded into a circular buffer.
//   2. At every loop boundary (e.g. every bar) a "voice" starts reading the
//      buffer from that point, but advancing at `rate` samples per sample.
//      rate 0.5 = half speed, an octave down. Because rate <= 1 the read
//      position can never overtake the write position, so there is no latency.
//   3. When the next boundary arrives, the old voice fades out while a new
//      one fades in (the "smooth" control sets the crossfade length).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dragtime
{

// ---------------------------------------------------------------------------
// EDIT HERE: speed modes. Add a line to add a mode. rate must be > 0 and <= 1
// (a real-time effect cannot play audio that has not happened yet).
// ---------------------------------------------------------------------------
struct SpeedMode { const char* name; double rate; };

inline constexpr std::array<SpeedMode, 5> kSpeedModes {{
    { "0.5x (half speed)",     0.5       },
    { "0.25x (quarter speed)", 0.25      },
    { "0.667x (triplet feel)", 2.0 / 3.0 },
    { "0.75x",                 0.75      },
    { "0.8x",                  0.8       },
}};

// ---------------------------------------------------------------------------
// EDIT HERE: loop lengths, in quarter-note beats (assumes 4/4 for "bar").
// ---------------------------------------------------------------------------
struct LoopLength { const char* name; double beats; };

inline constexpr std::array<LoopLength, 8> kLoopLengths {{
    { "1/16",   0.25 },
    { "1/8",    0.5  },
    { "1/4",    1.0  },
    { "1/2",    2.0  },
    { "1 bar",  4.0  },
    { "2 bars", 8.0  },
    { "4 bars", 16.0 },
    { "8 bars", 32.0 },
}};

// ---------------------------------------------------------------------------
// EDIT HERE: filter LFO rates (length of one LFO cycle, in quarter notes)
// and shapes. Shapes are implemented in Engine::lfoShape().
// ---------------------------------------------------------------------------
inline constexpr std::array<LoopLength, 9> kLfoRates {{
    { "8 bars", 32.0 },
    { "4 bars", 16.0 },
    { "2 bars", 8.0  },
    { "1 bar",  4.0  },
    { "1/2",    2.0  },
    { "1/4",    1.0  },
    { "1/8",    0.5  },
    { "1/8 triplet", 1.0 / 3.0 },
    { "1/16",   0.25 },
}};

inline constexpr std::array<const char*, 5> kLfoShapes {{
    "Sine", "Triangle", "Ramp Up", "Ramp Down", "Square",
}};

inline constexpr float kLfoMaxOctaves = 3.0f;   // cutoff swing at LFO depth = 1
inline constexpr float kMaxDriveDb    = 24.0f;  // input boost at drive = 1

inline constexpr double kMinBpm        = 40.0;  // slowest tempo the buffer is sized for
inline constexpr double kMaxLoopBeats  = 32.0;  // must be >= the longest entry above
inline constexpr float  kMaxSmoothMs   = 50.0f; // crossfade length when smooth = 1

struct Params
{
    bool   power     = true;     // effect on/off (fades using fadeInMs / fadeOutMs)
    double rate      = 0.5;      // playback rate, 0 < rate <= 1
    double loopBeats = 4.0;      // loop length in quarter notes
    float  mix       = 1.0f;     // 0 = dry, 1 = fully slowed
    float  smooth    = 0.3f;     // 0..1, crossfade at loop boundaries
    float  bandLoHz  = 20.0f;    // only frequencies between bandLoHz and
    float  bandHiHz  = 20000.0f; //   bandHiHz are slowed; the rest stays dry
    float  fadeInMs  = 20.0f;    // power-on fade
    float  fadeOutMs = 20.0f;    // power-off fade

    // Live filter on the slowed signal only (the dry signal is not filtered).
    // Both cutoffs are smoothed, so they can be swept or automated click-free.
    float  filterLoHz = 20.0f;    // low cut  (high-pass) on the effect
    float  filterHiHz = 20000.0f; // high cut (low-pass) on the effect
    float  filterRes  = 0.0f;     // 0..1 resonance at both cutoffs

    // One-knob DJ filter on the effect: 0 = open, toward -1 = low-pass
    // closing down, toward +1 = high-pass opening up. Stacks with the cuts above.
    float  djFilter   = 0.0f;

    // Tempo-synced LFO that moves both filter cutoffs up and down together.
    float  lfoDepth   = 0.0f;     // 0..1 (1 = +/- kLfoMaxOctaves)
    double lfoBeats   = 4.0;      // one cycle, in quarter notes
    int    lfoShape   = 0;        // index into kLfoShapes

    // Saturation on the slowed signal, before the filter.
    float  drive      = 0.0f;     // 0..1
};

class Engine
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        channels = std::clamp (numChannels, 1, kMaxChannels);

        const double maxSeconds = kMaxLoopBeats * 60.0 / kMinBpm + 1.0;
        size_t size = 1;
        while ((double) size < maxSeconds * sr) size <<= 1;
        mask = (int64_t) size - 1;

        for (int ch = 0; ch < channels; ++ch)
            buffer[(size_t) ch].assign (size, 0.0f);

        reset();
    }

    void reset()
    {
        for (int ch = 0; ch < channels; ++ch)
            std::fill (buffer[(size_t) ch].begin(), buffer[(size_t) ch].end(), 0.0f);

        writeCount = 0;
        lastLoopIndex = kNoLoop;
        voices = {};
        filters = {};
        powerGain = 0.0f;
        mixSmoothed = -1.0f;
        controlsInitialised = false;
        controlCounter = 0;
        lfoPhase = 0.0;
        lfoSmoothed = 0.0f;
    }

    // io: in-place audio. ppqStart: host position (quarter notes) of sample 0.
    void process (float* const* io, int numChannels, int numSamples,
                  const Params& p, double bpm, double ppqStart, bool playing)
    {
        if (mask == 0) return;

        numChannels = std::min (numChannels, channels);
        bpm = std::max (bpm, kMinBpm);

        const double rate = std::clamp (p.rate, 0.01, 1.0);
        const double loopBeats = std::clamp (p.loopBeats, 1.0 / 64.0, kMaxLoopBeats);
        const double beatsPerSample = bpm / (60.0 * sr);
        const double loopSamples = std::max (64.0, loopBeats / beatsPerSample);

        // Crossfade length: never more than a quarter of the loop.
        const double fadeSamples = std::max (1.0,
            std::min ((double) p.smooth * kMaxSmoothMs * 0.001 * sr, loopSamples * 0.25));
        const float fadeStep = (float) (1.0 / fadeSamples);

        // Cutoff targets (log2 Hz). The smoothed values chase these in
        // updateControls(), which is what makes live sweeps click-free.
        // The DJ knob adds its own low cut (right of centre) or high cut (left).
        float djLowCut = kLogMinHz, djHighCut = kLogMaxHz;
        {
            // EDIT HERE: dead zone at centre, and how far each side sweeps.
            const float dead = 0.03f;
            const float dj = std::clamp (p.djFilter, -1.0f, 1.0f);
            const float amount = std::max (0.0f, (std::abs (dj) - dead) / (1.0f - dead));
            if (dj > 0.0f) djLowCut  = kLogMinHz + amount * (std::log2 (8000.0f) - kLogMinHz);
            if (dj < 0.0f) djHighCut = kLogMaxHz - amount * (kLogMaxHz - std::log2 (80.0f));
        }

        const std::array<float, 4> cutoffTargets {
            logHz (p.bandLoHz), logHz (p.bandHiHz),
            std::max (logHz (p.filterLoHz), djLowCut),
            std::min (logHz (p.filterHiHz), djHighCut) };
        const float resonance = std::clamp (p.filterRes, 0.0f, 1.0f);
        const float driveTarget = std::clamp (p.drive, 0.0f, 1.0f);

        const float lfoDepthOctaves = std::clamp (p.lfoDepth, 0.0f, 1.0f) * kLfoMaxOctaves;
        const double lfoBeats = std::clamp (p.lfoBeats, 1.0 / 64.0, 256.0);

        const float powerTarget = p.power ? 1.0f : 0.0f;
        const float powerUp   = rampStep (p.fadeInMs);
        const float powerDown = rampStep (p.fadeOutMs);

        const float mixTarget = std::clamp (p.mix, 0.0f, 1.0f);
        if (mixSmoothed < 0.0f) mixSmoothed = mixTarget;
        const float mixCoeff = 1.0f - std::exp (-1.0f / (0.02f * (float) sr));

        for (int n = 0; n < numSamples; ++n)
        {
            // ---- loop boundary detection --------------------------------
            bool trigger = false;

            if (playing)
            {
                // Small epsilon so host rounding at block edges cannot make
                // the position flicker back across a boundary.
                const double ppq = ppqStart + n * beatsPerSample + 1.0e-6;
                const auto index = (int64_t) std::floor (ppq / loopBeats);
                if (index != lastLoopIndex) { trigger = true; lastLoopIndex = index; }
            }
            else
            {
                // Transport stopped: free-run at the current tempo.
                lastLoopIndex = kNoLoop;
                if (! voices[0].active || voices[0].phase >= loopSamples) trigger = true;
            }

            if (trigger)
            {
                voices[1] = voices[0];                     // old loop fades out
                voices[0] = { writeCount, 0.0, 0.0f, true }; // new loop fades in
            }

            // ---- smoothed controls --------------------------------------
            if (controlCounter == 0)
            {
                // LFO follows the song position while playing, free-runs otherwise.
                if (playing)
                {
                    const double cycles = (ppqStart + n * beatsPerSample) / lfoBeats;
                    lfoPhase = cycles - std::floor (cycles);
                }
                else
                {
                    lfoPhase += kControlInterval * beatsPerSample / lfoBeats;
                    lfoPhase -= std::floor (lfoPhase);
                }

                const float lfoOctaves = lfoDepthOctaves * lfoShape (p.lfoShape, (float) lfoPhase);
                updateControls (cutoffTargets, resonance, lfoOctaves, driveTarget);
            }
            controlCounter = (controlCounter + 1) % kControlInterval;

            if (powerGain < powerTarget)      powerGain = std::min (powerTarget, powerGain + powerUp);
            else if (powerGain > powerTarget) powerGain = std::max (powerTarget, powerGain - powerDown);

            mixSmoothed += (mixTarget - mixSmoothed) * mixCoeff;
            const float wetAmount = mixSmoothed * powerGain;

            // ---- voice gains (equal-power crossfade) --------------------
            voices[0].fade = std::min (1.0f, voices[0].fade + fadeStep);
            if (voices[1].active)
            {
                voices[1].fade -= fadeStep;
                if (voices[1].fade <= 0.0f) voices[1].active = false;
            }

            const float g0 = voices[0].active ? fadeCurve (voices[0].fade) : 0.0f;
            const float g1 = voices[1].active ? fadeCurve (voices[1].fade) : 0.0f;
            const double pos0 = (double) voices[0].start + voices[0].phase * rate;
            const double pos1 = (double) voices[1].start + voices[1].phase * rate;

            // ---- audio --------------------------------------------------
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float in = io[ch][n];

                auto& f = filters[(size_t) ch];

                // Band split: which part of the input gets slowed. The filters
                // always run so their state is ready when the band is narrowed.
                const float highPassed = in - f.lo.lowpass (in, gLo);
                const float split = f.hi.lowpass (highPassed, gHi);
                const float band = fullBand ? in : split;
                const float rest = in - band; // untouched part of the spectrum

                auto& buf = buffer[(size_t) ch];
                buf[(size_t) (writeCount & mask)] = band;

                float wet = 0.0f;
                if (g0 > 0.0f) wet += g0 * read (buf, pos0);
                if (g1 > 0.0f) wet += g1 * read (buf, pos1);

                // Drive: tanh saturation, blended in so drive = 0 is exactly clean.
                if (driveBlend > 0.0f)
                    wet += driveBlend * (std::tanh (wet * driveGain) * driveMakeup - wet);

                // Live filter on the slowed signal: high-pass, then low-pass.
                const float filtered = f.wetLp.lowpass (f.wetHp.highpass (wet, hpCoeffs), lpCoeffs);
                if (! filterOpen) wet = filtered;

                io[ch][n] = rest + band + wetAmount * (wet - band);
            }

            voices[0].phase += 1.0;
            voices[1].phase += 1.0;
            ++writeCount;
        }
    }

private:
    static constexpr int kMaxChannels = 2;
    static constexpr int64_t kNoLoop = INT64_MIN;

    struct Voice
    {
        int64_t start  = 0;     // absolute sample index where this loop began
        double  phase  = 0.0;   // samples elapsed since start
        float   fade   = 0.0f;  // 0..1 crossfade position
        bool    active = false;
    };

    // Two cascaded one-pole lowpasses (12 dB/oct), trapezoidal form.
    struct Lowpass2
    {
        float s1 = 0.0f, s2 = 0.0f;
        float lowpass (float x, float g)
        {
            float v = (x - s1) * g; float y = v + s1; s1 = y + v;
            v = (y - s2) * g; y = v + s2; s2 = y + v;
            return y;
        }
    };
    // Resonant 12 dB/oct state-variable filter (trapezoidal / TPT form).
    struct SvfCoeffs { float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.414f; };

    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f;

        float lowpass (float x, const SvfCoeffs& c)
        {
            const float v3 = x - ic2;
            const float v1 = c.a1 * ic1 + c.a2 * v3;
            const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return v2;
        }

        float highpass (float x, const SvfCoeffs& c)
        {
            const float v3 = x - ic2;
            const float v1 = c.a1 * ic1 + c.a2 * v3;
            const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return x - c.k * v1 - v2;
        }
    };

    struct ChannelFilters { Lowpass2 lo, hi; Svf wetHp, wetLp; };

    static float logHz (float hz) { return std::log2 (std::clamp (hz, 20.0f, 20000.0f)); }

    // phase 0..1 in, -1..+1 out. Add a case here and a name in kLfoShapes.
    static float lfoShape (int shape, float phase)
    {
        switch (shape)
        {
            case 1:  return 1.0f - 4.0f * std::abs (phase - 0.5f);   // triangle
            case 2:  return 2.0f * phase - 1.0f;                      // ramp up
            case 3:  return 1.0f - 2.0f * phase;                      // ramp down
            case 4:  return phase < 0.5f ? 1.0f : -1.0f;              // square
            default: return std::sin (6.2831853f * phase);            // sine
        }
    }

    SvfCoeffs svfCoeffs (float hz, float resonance) const
    {
        const double fc = std::clamp ((double) hz, 10.0, sr * 0.49);
        const double g = std::tan (3.14159265358979323846 * fc / sr);
        // EDIT HERE: resonance range. Q runs from 0.707 (flat) to 10 (sharp peak).
        const double q = 0.707 * std::pow (10.0 / 0.707, (double) resonance);
        const double k = 1.0 / q;
        SvfCoeffs c;
        c.a1 = (float) (1.0 / (1.0 + g * (g + k)));
        c.a2 = (float) (g * c.a1);
        c.a3 = (float) (g * c.a2);
        c.k  = (float) k;
        return c;
    }

    // Runs every kControlInterval samples: glides the four cutoffs toward
    // their targets and recalculates filter coefficients.
    void updateControls (const std::array<float, 4>& targets, float resonance,
                         float lfoOctaves, float driveTarget)
    {
        if (! controlsInitialised)
        {
            cutoffs = targets;
            resSmoothed = resonance;
            driveSmoothed = driveTarget;
            lfoSmoothed = lfoOctaves;
            controlsInitialised = true;
        }

        // EDIT HERE: 0.03 = glide time in seconds for filter sweeps.
        const float c = 1.0f - std::exp (-(float) kControlInterval / (0.03f * (float) sr));
        for (size_t i = 0; i < cutoffs.size(); ++i)
            cutoffs[i] += (targets[i] - cutoffs[i]) * c;
        resSmoothed += (resonance - resSmoothed) * c;
        driveSmoothed += (driveTarget - driveSmoothed) * c;

        // The LFO gets a much shorter glide (5 ms): enough to stop the square
        // and ramp shapes clicking without blurring fast rates.
        const float lfoC = 1.0f - std::exp (-(float) kControlInterval / (0.005f * (float) sr));
        lfoSmoothed += (lfoOctaves - lfoSmoothed) * lfoC;

        const float bandLo = std::exp2 (cutoffs[0]);
        const float bandHi = std::max (std::exp2 (cutoffs[1]), bandLo);
        const float filtLo = std::exp2 (std::clamp (cutoffs[2] + lfoSmoothed, kLogMinHz, kLogMaxHz));
        const float filtHi = std::max (std::exp2 (std::clamp (cutoffs[3] + lfoSmoothed, kLogMinHz, kLogMaxHz)), filtLo);

        // EDIT HERE: drive curve. driveGain pushes into tanh; driveMakeup pulls
        // the level back so the knob changes tone more than volume.
        driveGain   = std::pow (10.0f, driveSmoothed * kMaxDriveDb / 20.0f);
        driveMakeup = std::pow (driveGain, -0.5f);
        driveBlend  = driveSmoothed < 1.0e-4f ? 0.0f : std::min (1.0f, driveSmoothed * 8.0f);

        gLo = onePoleCoeff (bandLo);
        gHi = onePoleCoeff (bandHi);
        fullBand = bandLo <= 20.5f && bandHi >= 19990.0f;

        hpCoeffs = svfCoeffs (filtLo, resSmoothed);
        lpCoeffs = svfCoeffs (filtHi, resSmoothed);
        filterOpen = filtLo <= 20.5f && filtHi >= 19990.0f;
    }

    float onePoleCoeff (float hz) const
    {
        const double fc = std::clamp ((double) hz, 10.0, sr * 0.49);
        const double g = std::tan (3.14159265358979323846 * fc / sr);
        return (float) (g / (1.0 + g));
    }

    float rampStep (float ms) const
    {
        return ms <= 0.0f ? 1.0f : (float) (1000.0 / ((double) ms * sr));
    }

    static float fadeCurve (float t)
    {
        return t >= 1.0f ? 1.0f : std::sin (1.5707963f * t);
    }

    float read (const std::vector<float>& buf, double pos) const
    {
        const auto i = (int64_t) pos;
        const float frac = (float) (pos - (double) i);
        const float a = buf[(size_t) (i & mask)];
        const float b = buf[(size_t) ((i + 1) & mask)];
        return a + frac * (b - a);
    }

    double sr = 44100.0;
    int channels = 2;
    int64_t mask = 0;
    int64_t writeCount = 0;
    int64_t lastLoopIndex = kNoLoop;

    std::array<std::vector<float>, kMaxChannels> buffer;
    std::array<Voice, 2> voices {};
    std::array<ChannelFilters, kMaxChannels> filters {};

    float powerGain = 0.0f;
    float mixSmoothed = -1.0f;

    static constexpr int kControlInterval = 8;
    int   controlCounter = 0;
    bool  controlsInitialised = false;
    std::array<float, 4> cutoffs {};   // smoothed log2 Hz: bandLo, bandHi, filterLo, filterHi
    float resSmoothed = 0.0f;
    float driveSmoothed = 0.0f, driveGain = 1.0f, driveMakeup = 1.0f, driveBlend = 0.0f;
    double lfoPhase = 0.0;
    float lfoSmoothed = 0.0f;
    static constexpr float kLogMinHz = 4.321928f;   // log2 (20)
    static constexpr float kLogMaxHz = 14.287712f;  // log2 (20000)
    float gLo = 0.0f, gHi = 1.0f;
    bool  fullBand = true, filterOpen = true;
    SvfCoeffs hpCoeffs, lpCoeffs;
};

} // namespace dragtime
