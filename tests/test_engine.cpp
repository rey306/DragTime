// Standalone check of the DSP, no JUCE needed:
//   g++ -std=c++17 -O2 -I../Source test_engine.cpp -o test_engine && ./test_engine
#include "Engine.h"
#include <cstdio>
#include <vector>

using namespace dragtime;

static const double kSr = 48000.0, kBpm = 120.0, kPi = 3.14159265358979323846;
static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

// Runs a sine through the engine in 512-sample blocks, returns the output.
static std::vector<float> run (Params p, double hz, double seconds, bool playing,
                               std::vector<float>* dryOut = nullptr)
{
    Engine e; e.prepare (kSr, 2);
    const int total = (int) (seconds * kSr);
    std::vector<float> l ((size_t) total), r ((size_t) total);
    for (int i = 0; i < total; ++i)
        l[(size_t) i] = r[(size_t) i] = (float) std::sin (2.0 * kPi * hz * i / kSr);
    if (dryOut) *dryOut = l;

    for (int start = 0; start < total; start += 512)
    {
        const int n = std::min (512, total - start);
        float* io[2] = { l.data() + start, r.data() + start };
        e.process (io, 2, n, p, kBpm, start * kBpm / (60.0 * kSr), playing);
    }
    return l;
}

static double frequency (const std::vector<float>& x, int from, int to)
{
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if (x[(size_t) i - 1] < 0.0f && x[(size_t) i] >= 0.0f) ++crossings;
    return crossings * kSr / (to - from);
}

int main()
{
    for (const auto& mode : kSpeedModes)
    {
        Params p; p.rate = mode.rate; p.fadeInMs = 0.0f;
        auto out = run (p, 440.0, 4.0, true);
        const double f = frequency (out, 4800, (int) out.size());
        char label[128];
        std::snprintf (label, sizeof label, "%-24s 440 Hz in -> %.1f Hz out (expect %.1f)",
                       mode.name, f, 440.0 * mode.rate);
        check (std::abs (f - 440.0 * mode.rate) < 3.0, label);
    }

    {   // transport stopped: still slows, free-running
        Params p; p.fadeInMs = 0.0f;
        auto out = run (p, 440.0, 4.0, false);
        check (std::abs (frequency (out, 4800, (int) out.size()) - 220.0) < 3.0, "works with transport stopped");
    }
    {   // mix 0 and power off are both bit-transparent
        std::vector<float> dry;
        Params p; p.mix = 0.0f;
        auto out = run (p, 440.0, 2.0, true, &dry);
        float err = 0.0f;
        for (size_t i = 0; i < out.size(); ++i) err = std::max (err, std::abs (out[i] - dry[i]));
        check (err < 1.0e-6f, "mix = 0 passes the dry signal");

        Params q; q.power = false;
        out = run (q, 440.0, 2.0, true, &dry);
        err = 0.0f;
        for (size_t i = 0; i < out.size(); ++i) err = std::max (err, std::abs (out[i] - dry[i]));
        check (err < 1.0e-6f, "power off passes the dry signal");
    }
    {   // band split: 100 Hz tone outside a 1k-20k band must stay at 100 Hz
        Params p; p.fadeInMs = 0.0f; p.bandLoHz = 1000.0f;
        auto out = run (p, 100.0, 4.0, true);
        check (std::abs (frequency (out, 4800, (int) out.size()) - 100.0) < 2.0, "band split leaves out-of-band audio alone");
    }
    {   // output stays finite and bounded, with smoothing off and on
        bool ok = true;
        for (float s : { 0.0f, 1.0f })
            for (const auto& len : kLoopLengths)
            {
                Params p; p.smooth = s; p.loopBeats = len.beats;
                for (float v : run (p, 440.0, 3.0, true))
                    if (! std::isfinite (v) || std::abs (v) > 1.5f) ok = false;
            }
        check (ok, "output finite and bounded for every loop length");
    }

    {   // live filter: band-passing 2k-20k must remove a 220 Hz slowed tone
        auto rms = [] (const std::vector<float>& x) {
            double sum = 0.0;
            for (size_t i = x.size() / 2; i < x.size(); ++i) sum += (double) x[i] * x[i];
            return std::sqrt (sum / (double) (x.size() / 2));
        };
        Params open; open.fadeInMs = 0.0f;
        Params cut = open; cut.filterLoHz = 2000.0f;
        const double a = rms (run (open, 440.0, 3.0, true)), b = rms (run (cut, 440.0, 3.0, true));
        char label[128];
        std::snprintf (label, sizeof label, "live filter cuts the effect (%.3f -> %.3f RMS)", a, b);
        check (b < a * 0.05, label);
    }
    {   // sweeping both cutoffs every block with full resonance: stable, no jumps
        Engine e; e.prepare (kSr, 2);
        const int total = (int) (4.0 * kSr);
        std::vector<float> l ((size_t) total), r ((size_t) total);
        for (int i = 0; i < total; ++i)
            l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 440.0 * i / kSr);
        bool ok = true; float maxStep = 0.0f, prev = 0.0f;
        for (int start = 0; start < total; start += 64)
        {
            Params p; p.filterRes = 1.0f;
            const double lfo = 0.5 + 0.5 * std::sin (2.0 * kPi * 3.0 * start / kSr);
            p.filterLoHz = (float) (20.0 * std::pow (100.0, lfo));    // 20 Hz .. 2 kHz
            p.filterHiHz = (float) (20000.0 / std::pow (40.0, lfo));  // 20 kHz .. 500 Hz
            const int n = std::min (64, total - start);
            float* io[2] = { l.data() + start, r.data() + start };
            e.process (io, 2, n, p, kBpm, start * kBpm / (60.0 * kSr), true);
            for (int i = 0; i < n; ++i)
            {
                const float v = l[(size_t) (start + i)];
                if (! std::isfinite (v) || std::abs (v) > 8.0f) ok = false;
                if (start + i > 4800) maxStep = std::max (maxStep, std::abs (v - prev));
                prev = v;
            }
        }
        char label[128];
        std::snprintf (label, sizeof label, "fast filter sweep at full resonance is stable (max step %.3f)", maxStep);
        check (ok && maxStep < 0.5f, label);
    }

    {
        auto rms = [] (const std::vector<float>& x) {
            double sum = 0.0;
            for (size_t i = x.size() / 2; i < x.size(); ++i) sum += (double) x[i] * x[i];
            return std::sqrt (sum / (double) (x.size() / 2));
        };
        char label[160];
        Params clean; clean.fadeInMs = 0.0f;

        // DJ knob: right of centre high-passes (kills a 220 Hz slowed tone)...
        Params hp = clean; hp.djFilter = 0.9f;
        double a = rms (run (clean, 440.0, 3.0, true)), b = rms (run (hp, 440.0, 3.0, true));
        std::snprintf (label, sizeof label, "DJ knob right = high-pass (%.3f -> %.3f RMS)", a, b);
        check (b < a * 0.05, label);

        // ...left of centre low-passes (kills a 4 kHz slowed tone)...
        Params lp = clean; lp.djFilter = -0.9f;
        a = rms (run (clean, 8000.0, 3.0, true)); b = rms (run (lp, 8000.0, 3.0, true));
        std::snprintf (label, sizeof label, "DJ knob left = low-pass (%.3f -> %.3f RMS)", a, b);
        check (b < a * 0.05, label);

        // ...and centre is exactly the same as no filter.
        Params centre = clean; centre.djFilter = 0.02f;
        check (run (clean, 440.0, 2.0, true) == run (centre, 440.0, 2.0, true), "DJ knob centre is fully open");

        // Drive: bounded. A fixed saturator cannot hold level for every input
        // (quiet audio comes up, full-scale audio comes down), so allow 10 dB.
        bool ok = true; double lo = 1e9, hi = 0.0;
        for (float d : { 0.1f, 0.25f, 0.5f, 0.75f, 1.0f })
            for (float amp : { 0.1f, 1.0f })
            {
                Engine e; e.prepare (kSr, 2);
                const int total = (int) (2.0 * kSr);
                std::vector<float> l ((size_t) total), r ((size_t) total);
                for (int i = 0; i < total; ++i)
                    l[(size_t) i] = r[(size_t) i] = amp * (float) std::sin (2.0 * kPi * 440.0 * i / kSr);
                Params p = clean; p.drive = d;
                float* io[2] = { l.data(), r.data() };
                e.process (io, 2, total, p, kBpm, 0.0, true);
                for (float v : l) if (! std::isfinite (v) || std::abs (v) > 1.5f) ok = false;
                const double ratio = rms (l) / (amp * 0.7071);
                lo = std::min (lo, ratio); hi = std::max (hi, ratio);
            }
        std::snprintf (label, sizeof label, "drive is bounded, level change %.1f to %+.1f dB",
                       20.0 * std::log10 (lo), 20.0 * std::log10 (hi));
        check (ok && lo > 0.316 && hi < 3.16, label);

        // LFO: every shape and rate stays stable and actually changes the sound.
        ok = true; bool changes = true;
        const auto reference = run (clean, 440.0, 3.0, true);
        for (int shape = 0; shape < (int) kLfoShapes.size(); ++shape)
            for (const auto& rate : kLfoRates)
            {
                Params p = clean; p.lfoDepth = 1.0f; p.lfoShape = shape; p.lfoBeats = rate.beats; p.filterRes = 1.0f;
                for (bool playing : { true, false })
                {
                    const auto out = run (p, 440.0, 3.0, playing);
                    float prev = 0.0f;
                    for (size_t i = 0; i < out.size(); ++i)
                    {
                        if (! std::isfinite (out[i]) || std::abs (out[i]) > 12.0f) ok = false;
                        if (i > 4800 && std::abs (out[i] - prev) > 1.5f) ok = false;
                        prev = out[i];
                    }
                    if (playing && out == reference) changes = false;
                }
            }
        check (ok, "filter LFO stable for every shape and rate (playing and stopped)");
        check (changes, "filter LFO audibly modulates the effect");
    }

    std::printf (failures == 0 ? "\nAll tests passed.\n" : "\n%d test(s) failed.\n", failures);
    return failures;
}
