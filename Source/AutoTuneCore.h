// AutoTuneCore.h
// Motor de autotune sin dependencias (solo C++17 estándar).
//   1) PitchDetector : YIN sobre audio diezmado (~16 kHz), mediana de 3 estimaciones
//   2) Cuantización  : nota más cercana de la escala elegida, con histéresis
//   3) PitchShifter  : dos cabezales de lectura sobre una línea de retardo, con la
//                      ventana igual a un número PAR de periodos (estilo PSOLA), de
//                      modo que los saltos de lectura caen siempre en fase.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace at
{

// ----------------------------------------------------------------------------
// Escalas
// ----------------------------------------------------------------------------
inline constexpr int kNumScales = 8;

inline constexpr const char* kScaleNames[kNumScales] = {
    "Cromatica", "Mayor", "Menor natural", "Menor armonica",
    "Pentatonica mayor", "Pentatonica menor", "Blues", "Dorica"
};

// Máscara de 12 bits relativa a la tónica (bit i = intervalo de i semitonos)
inline constexpr int kScaleMasks[kNumScales] = {
    0b111111111111,                                   // cromática
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   // mayor
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   // menor natural
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),   // menor armónica
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                          // pentatónica mayor
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                         // pentatónica menor
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),              // blues
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10)    // dórica
};

// ----------------------------------------------------------------------------
// Detector de tono (YIN)
// ----------------------------------------------------------------------------
class PitchDetector
{
public:
    struct Result
    {
        bool  voiced = false;
        float f0 = 0.0f;       // Hz
    };

    void prepare (double sampleRate)
    {
        factor_ = std::max (1, (int) std::lround (sampleRate / 16000.0));
        dsr_ = sampleRate / (double) factor_;

        minTau_ = std::max (2, (int) std::floor (dsr_ / kFMax));
        maxTau_ = (int) std::ceil (dsr_ / kFMin) + 1;
        window_ = 512;
        hop_ = 128;

        ring_.assign (4096, 0.0f);
        ringMask_ = (int) ring_.size() - 1;
        linear_.assign ((size_t) (window_ + maxTau_ + 2), 0.0f);
        d_.assign ((size_t) maxTau_ + 2, 0.0f);
        cmnd_.assign ((size_t) maxTau_ + 2, 1.0f);
        reset();
    }

    void reset()
    {
        std::fill (ring_.begin(), ring_.end(), 0.0f);
        widx_ = 0;
        hopCount_ = 0;
        accCount_ = 0;
        acc_ = 0.0f;
        histCount_ = 0;
    }

    // Mete una muestra. Devuelve true cuando hay un resultado nuevo en 'out'.
    bool push (float x, Result& out)
    {
        acc_ += x;
        if (++accCount_ < factor_)
            return false;

        const float v = acc_ / (float) factor_;
        acc_ = 0.0f;
        accCount_ = 0;

        ring_[(size_t) (widx_ & ringMask_)] = v;
        ++widx_;

        if (++hopCount_ < hop_)
            return false;

        hopCount_ = 0;
        out = analyse();
        return true;
    }

private:
    static constexpr double kFMin = 70.0;
    static constexpr double kFMax = 1000.0;
    static constexpr float  kThreshold = 0.25f;

    Result analyse()
    {
        Result r;
        const int total = window_ + maxTau_;

        for (int i = 0; i < total; ++i)
            linear_[(size_t) i] = ring_[(size_t) ((widx_ - total + i) & ringMask_)];

        // Puerta de energía
        double e = 0.0;
        for (int i = total - window_; i < total; ++i)
            e += (double) linear_[(size_t) i] * linear_[(size_t) i];
        if (std::sqrt (e / window_) < 0.004)
            return r;

        // Función de diferencia
        const float* x = linear_.data();
        for (int tau = 1; tau <= maxTau_; ++tau)
        {
            float sum = 0.0f;
            for (int j = 0; j < window_; ++j)
            {
                const float diff = x[j] - x[j + tau];
                sum += diff * diff;
            }
            d_[(size_t) tau] = sum;
        }

        // Diferencia media acumulada normalizada
        cmnd_[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= maxTau_; ++tau)
        {
            running += d_[(size_t) tau];
            cmnd_[(size_t) tau] = running > 1e-12f ? d_[(size_t) tau] * (float) tau / running : 1.0f;
        }

        // Primer valle por debajo del umbral
        int tau = -1;
        for (int t = minTau_; t < maxTau_; ++t)
        {
            if (cmnd_[(size_t) t] < kThreshold)
            {
                while (t + 1 < maxTau_ && cmnd_[(size_t) t + 1] < cmnd_[(size_t) t])
                    ++t;
                tau = t;
                break;
            }
        }
        if (tau < 0)
            return r;

        // Interpolación parabólica
        double better = tau;
        if (tau > 1 && tau < maxTau_)
        {
            const double s0 = cmnd_[(size_t) tau - 1], s1 = cmnd_[(size_t) tau], s2 = cmnd_[(size_t) tau + 1];
            const double denom = s0 - 2.0 * s1 + s2;
            if (std::abs (denom) > 1e-12)
                better = tau + 0.5 * (s0 - s2) / denom;
        }

        float f0 = (float) (dsr_ / better);
        if (f0 < kFMin || f0 > kFMax)
            return r;

        // Mediana de las últimas 3 estimaciones (quita saltos de octava sueltos)
        hist_[histCount_ % 3] = f0;
        ++histCount_;
        if (histCount_ >= 3)
        {
            float a = hist_[0], b = hist_[1], c = hist_[2];
            f0 = std::max (std::min (a, b), std::min (std::max (a, b), c));
        }

        r.voiced = true;
        r.f0 = f0;
        return r;
    }

    int factor_ = 3;
    double dsr_ = 16000.0;
    int minTau_ = 16, maxTau_ = 230, window_ = 512, hop_ = 128;
    std::vector<float> ring_, linear_, d_, cmnd_;
    int ringMask_ = 4095, widx_ = 0, hopCount_ = 0, accCount_ = 0;
    float acc_ = 0.0f;
    float hist_[3] = { 0, 0, 0 };
    int histCount_ = 0;
};

// ----------------------------------------------------------------------------
// Cambio de tono (dos cabezales + ventana de N periodos, N par)
// ----------------------------------------------------------------------------
class PitchShifter
{
public:
    void prepare (double sampleRate)
    {
        sr_ = sampleRate;
        buf_.assign (32768, 0.0f);
        mask_ = (int) buf_.size() - 1;
        minWindow_ = 0.030 * sr_;
        maxWindow_ = 8000.0;
        reset();
    }

    void reset()
    {
        std::fill (buf_.begin(), buf_.end(), 0.0f);
        wpos_ = 0;
        phase_ = 0.5;
        window_ = minWindow_;
    }

    // ratio > 1 sube el tono. period = periodo de la voz en muestras.
    // idle = true cuando no hay voz: el cabezal vuelve despacio al centro (latencia fija)
    float process (float x, double ratio, double period, bool idle)
    {
        buf_[(size_t) wpos_] = x;

        // Ventana = número par de periodos, lo más cercana posible a minWindow_
        double target = minWindow_;
        if (period > 20.0 && period < maxWindow_ * 0.5)
        {
            const int k = 2 * std::max (1, (int) std::lround (minWindow_ / (2.0 * period)));
            target = std::min (maxWindow_, k * period);
        }
        window_ += 0.002 * (target - window_);

        phase_ += (1.0 - ratio) / window_;
        if (idle)
        {
            const double step = 0.05 / window_;
            const double diff = 0.5 - phase_;
            phase_ += std::max (-step, std::min (step, diff));
        }
        phase_ -= std::floor (phase_);

        float y = 0.0f;
        for (int h = 0; h < 2; ++h)
        {
            double ph = phase_ + 0.5 * h;
            ph -= std::floor (ph);
            const double s = std::sin (3.14159265358979323846 * ph);
            y += (float) (s * s) * read (ph * window_);
        }

        wpos_ = (wpos_ + 1) & mask_;
        return y;
    }

    double nominalLatency() const { return 0.5 * minWindow_; }

private:
    float read (double delay) const
    {
        const double rpos = (double) wpos_ - delay;
        const double fl = std::floor (rpos);
        const float frac = (float) (rpos - fl);
        const int i0 = (int) fl;
        const float a = buf_[(size_t) (i0 & mask_)];
        const float b = buf_[(size_t) ((i0 + 1) & mask_)];
        return a + frac * (b - a);
    }

    double sr_ = 44100.0, minWindow_ = 1323.0, maxWindow_ = 8000.0;
    double phase_ = 0.5, window_ = 1323.0;
    std::vector<float> buf_;
    int mask_ = 32767, wpos_ = 0;
};

// ----------------------------------------------------------------------------
// Motor completo
// ----------------------------------------------------------------------------
class Engine
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate, int numChannels)
    {
        sr_ = sampleRate;
        channels_ = std::max (1, std::min (kMaxChannels, numChannels));
        detector_.prepare (sampleRate);
        for (auto& s : shifter_)
            s.prepare (sampleRate);
        reset();
    }

    void reset()
    {
        detector_.reset();
        for (auto& s : shifter_)
            s.reset();
        corrSmoothed_ = 0.0;
        corrTarget_ = 0.0;
        period_ = sr_ / 200.0;
        voiced_ = false;
        unvoicedCount_ = 100;
        currentNote_ = -1;
        detectedHz_ = 0.0f;
        targetNote_ = -1;
    }

    // key: 0..11 (C..B), scale: índice en kScaleMasks, speedMs: 0..400, amount: 0..1
    void setParams (int key, int scale, float speedMs, float amount)
    {
        key_ = ((key % 12) + 12) % 12;
        scale_ = std::max (0, std::min (kNumScales - 1, scale));
        speedMs_ = speedMs;
        amount_ = std::max (0.0f, std::min (1.0f, amount));
    }

    int latencySamples() const { return (int) std::lround (shifter_[0].nominalLatency()); }
    float detectedHz() const { return detectedHz_; }
    int targetNote() const { return targetNote_; }

    // Procesa in-place. io[c] apunta al canal c.
    void process (float* const* io, int numChannels, int numSamples)
    {
        const int nch = std::min (numChannels, channels_);
        const double coeff = speedMs_ <= 0.5f ? 1.0 : 1.0 - std::exp (-1.0 / ((double) speedMs_ * 0.001 * sr_));

        for (int i = 0; i < numSamples; ++i)
        {
            float mono = 0.0f;
            for (int c = 0; c < nch; ++c)
                mono += io[c][i];
            mono /= (float) nch;

            PitchDetector::Result r;
            if (detector_.push (mono, r))
                onDetection (r);

            corrSmoothed_ += coeff * (corrTarget_ - corrSmoothed_);
            const double ratio = std::pow (2.0, corrSmoothed_ / 12.0);
            const bool idle = ! voiced_;

            for (int c = 0; c < nch; ++c)
                io[c][i] = shifter_[c].process (io[c][i], ratio, period_, idle);
        }
    }

private:
    void onDetection (const PitchDetector::Result& r)
    {
        if (r.voiced)
        {
            unvoicedCount_ = 0;
            voiced_ = true;
            period_ = sr_ / (double) r.f0;
            detectedHz_ = r.f0;

            const double m = 69.0 + 12.0 * std::log2 ((double) r.f0 / 440.0);
            const int best = nearestAllowed (m);

            // Histéresis: no cambiar de nota por muy poco
            if (currentNote_ < 0 || ! isAllowed (currentNote_)
                || std::abs (m - best) < std::abs (m - currentNote_) - 0.15)
                currentNote_ = best;

            targetNote_ = currentNote_;
            corrTarget_ = ((double) currentNote_ - m) * (double) amount_;
        }
        else
        {
            if (++unvoicedCount_ >= 3)
            {
                voiced_ = false;
                currentNote_ = -1;
                targetNote_ = -1;
                corrTarget_ = 0.0;
                detectedHz_ = 0.0f;
            }
        }
    }

    bool isAllowed (int note) const
    {
        const int rel = (((note - key_) % 12) + 12) % 12;
        return (kScaleMasks[scale_] >> rel) & 1;
    }

    int nearestAllowed (double m) const
    {
        const int centre = (int) std::lround (m);
        int best = centre;
        double bestDist = 1e9;
        for (int n = centre - 7; n <= centre + 7; ++n)
        {
            if (! isAllowed (n))
                continue;
            const double dist = std::abs ((double) n - m);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = n;
            }
        }
        return best;
    }

    double sr_ = 44100.0;
    int channels_ = 2;
    PitchDetector detector_;
    PitchShifter shifter_[kMaxChannels];

    int key_ = 0, scale_ = 0;
    float speedMs_ = 20.0f, amount_ = 1.0f;

    double corrSmoothed_ = 0.0, corrTarget_ = 0.0, period_ = 220.0;
    bool voiced_ = false;
    int unvoicedCount_ = 100, currentNote_ = -1, targetNote_ = -1;
    float detectedHz_ = 0.0f;
};

} // namespace at
