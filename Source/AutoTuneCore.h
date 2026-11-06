// AutoTuneCore.h
// Motor de autotune sin dependencias (solo C++17 estándar).
//   1) PitchDetector : YIN sobre audio diezmado (~16 kHz), ventana corta y reciente
//   2) Cuantización  : nota más cercana de la escala elegida, con histéresis
//   3) PitchShifter  : dos cabezales de lectura sobre una línea de retardo con ventana
//                      de EXACTAMENTE 2 periodos (como PSOLA): los dos cabezales quedan
//                      siempre a un periodo de distancia y suenan en fase.
//
// Notas de diseño (v2):
//   - La ventana es continua (2 x periodo) con límite de variación: no hay saltos de
//     tamaño al cruzar de nota, que era lo que producía los "ruidos raros".
//   - El audio se retrasa unos ms (pre-retardo) para que la corrección calculada con la
//     detección llegue justo cuando suena esa parte del audio (aplana el vibrato).
//   - Sin voz, el cabezal vuelve rápido a la posición central (un solo cabezal activo):
//     consonantes y respiración pasan sin peinado ni barridos.
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
// Detector de tono (YIN con ventana reciente)
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
        window_ = 384;                   // ~24 ms
        hop_ = 64;                       // ~4 ms

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
    static constexpr float  kThreshold = 0.22f;

    Result analyse()
    {
        Result r;
        const int total = window_ + maxTau_;

        for (int i = 0; i < total; ++i)
            linear_[(size_t) i] = ring_[(size_t) ((widx_ - total + i) & ringMask_)];

        const float* y = linear_.data();
        const int base = total - window_;          // la ventana termina en la muestra más reciente

        // Puerta de energía
        double e = 0.0;
        for (int i = base; i < total; ++i)
            e += (double) y[i] * y[i];
        if (std::sqrt (e / window_) < 0.004)
            return r;

        // Función de diferencia sobre la ventana más reciente
        for (int tau = 1; tau <= maxTau_; ++tau)
        {
            float sum = 0.0f;
            for (int i = base; i < total; ++i)
            {
                const float diff = y[i] - y[i - tau];
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

        // Mediana de las últimas 3 estimaciones (quita saltos sueltos)
        hist_[histCount_ % 3] = f0;
        ++histCount_;
        if (histCount_ >= 3)
        {
            const float a = hist_[0], b = hist_[1], c = hist_[2];
            f0 = std::max (std::min (a, b), std::min (std::max (a, b), c));
        }

        r.voiced = true;
        r.f0 = f0;
        return r;
    }

    int factor_ = 3;
    double dsr_ = 16000.0;
    int minTau_ = 16, maxTau_ = 230, window_ = 384, hop_ = 64;
    std::vector<float> ring_, linear_, d_, cmnd_;
    int ringMask_ = 4095, widx_ = 0, hopCount_ = 0, accCount_ = 0;
    float acc_ = 0.0f;
    float hist_[3] = { 0, 0, 0 };
    int histCount_ = 0;
};

// ----------------------------------------------------------------------------
// Cambio de tono: dos cabezales, ventana = 2 periodos (continua)
// ----------------------------------------------------------------------------
class PitchShifter
{
public:
    static constexpr double kPreDelaySec = 0.009;     // alinea detección y audio
    static constexpr double kNominalHeadSec = 0.0065; // retardo medio del cabezal

    void prepare (double sampleRate)
    {
        sr_ = sampleRate;
        buf_.assign (65536, 0.0f);
        mask_ = (int) buf_.size() - 1;
        preDelay_ = kPreDelaySec * sr_;
        minWindow_ = 0.003 * sr_;          // ~330 Hz como mínimo de ventana
        maxWindow_ = 0.032 * sr_;          // 2 periodos a ~62 Hz
        windowSmooth_ = 1.0 - std::exp (-1.0 / (0.004 * sr_));
        buildTable();
        reset();
    }

    void reset()
    {
        std::fill (buf_.begin(), buf_.end(), 0.0f);
        wpos_ = 0;
        phase_ = 0.5;
        window_ = 0.010 * sr_;
    }

    // ratio > 1 sube el tono. period = periodo de la voz en muestras.
    // idle = true cuando no hay voz: el cabezal vuelve al centro (un solo cabezal activo)
    float process (float x, double ratio, double period, bool idle)
    {
        buf_[(size_t) wpos_] = x;

        // Ventana = 2 periodos, con variación limitada (sin saltos)
        const double target = std::max (minWindow_, std::min (maxWindow_, 2.0 * period));
        const double diff = target - window_;
        const double limit = 0.001 * window_;
        window_ += std::max (-limit, std::min (limit, windowSmooth_ * diff));

        phase_ += (1.0 - ratio) / window_;
        if (idle)
        {
            const double step = 0.12 / window_;
            const double d = 0.5 - phase_;
            phase_ += std::max (-step, std::min (step, d));
        }
        phase_ -= std::floor (phase_);

        float y = 0.0f;
        for (int h = 0; h < 2; ++h)
        {
            double ph = phase_ + 0.5 * h;
            ph -= std::floor (ph);
            const double s = std::sin (3.14159265358979323846 * ph);
            y += (float) (s * s) * read (preDelay_ + ph * window_);
        }

        wpos_ = (wpos_ + 1) & mask_;
        return y;
    }

    double nominalLatency() const { return preDelay_ + kNominalHeadSec * sr_; }

private:
    // Interpolación por sinc con ventana (8 puntos): conserva los agudos (la "s", la
    // respiración) incluso con retardos fraccionarios. La lineal los apagaba y modulaba.
    float read (double delay) const
    {
        const double rpos = (double) wpos_ - delay;
        const double fl = std::floor (rpos);
        const int phaseIdx = std::min (kPhases, (int) ((rpos - fl) * kPhases + 0.5));
        const int i0 = (int) fl - 3;
        const float* w = &table_[(size_t) phaseIdx * kTaps];
        float sum = 0.0f;
        for (int t = 0; t < kTaps; ++t)
            sum += w[t] * buf_[(size_t) ((i0 + t) & mask_)];
        return sum;
    }

    void buildTable()
    {
        const double pi = 3.14159265358979323846;
        table_.assign ((size_t) (kPhases + 1) * kTaps, 0.0f);
        for (int p = 0; p <= kPhases; ++p)
        {
            const double frac = (double) p / kPhases;
            double row[kTaps], total = 0.0;
            for (int t = 0; t < kTaps; ++t)
            {
                const double x = (double) t - 3.0 - frac;          // distancia a la posición de lectura
                const double sinc = std::abs (x) < 1e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
                const double u = x / 4.0;                          // ventana Blackman-Harris en [-4, 4]
                const double win = std::abs (u) >= 1.0 ? 0.0
                    : 0.35875 + 0.48829 * std::cos (pi * u) + 0.14128 * std::cos (2.0 * pi * u) + 0.01168 * std::cos (3.0 * pi * u);
                row[t] = sinc * win;
                total += row[t];
            }
            for (int t = 0; t < kTaps; ++t)
                table_[(size_t) p * kTaps + (size_t) t] = (float) (row[t] / total);
        }
    }

    static constexpr int kTaps = 8, kPhases = 256;
    std::vector<float> table_;
    double sr_ = 44100.0, preDelay_ = 400.0, minWindow_ = 132.0, maxWindow_ = 1411.0, windowSmooth_ = 0.005;
    double phase_ = 0.5, window_ = 441.0;
    std::vector<float> buf_;
    int mask_ = 65535, wpos_ = 0;
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
        voicedStreak_ = 0;
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
        // Mínimo 2 ms de suavizado: evita escalones audibles aunque la velocidad sea 0
        const double tauMs = std::max (2.0, (double) speedMs_);
        const double coeff = 1.0 - std::exp (-1.0 / (tauMs * 0.001 * sr_));

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
            const double ratio = std::exp2 (corrSmoothed_ / 12.0);
            const bool idle = ! voiced_;

            for (int c = 0; c < nch; ++c)
                io[c][i] = shifter_[c].process (io[c][i], ratio, period_, idle);
        }
    }

private:
    static constexpr double kMaxCorrectionSemitones = 3.5;

    void onDetection (const PitchDetector::Result& r)
    {
        if (r.voiced)
        {
            unvoicedCount_ = 0;
            ++voicedStreak_;
            period_ += 0.5 * ((sr_ / (double) r.f0) - period_);   // periodo suavizado
            if (voicedStreak_ == 1)
                period_ = sr_ / (double) r.f0;

            // Hace falta que la voz se confirme en 2 estimaciones seguidas
            if (voicedStreak_ < 2)
                return;

            voiced_ = true;
            detectedHz_ = r.f0;

            const double m = 69.0 + 12.0 * std::log2 ((double) r.f0 / 440.0);
            const int best = nearestAllowed (m);

            // Histéresis: no cambiar de nota por muy poco
            if (currentNote_ < 0 || ! isAllowed (currentNote_)
                || std::abs (m - best) < std::abs (m - currentNote_) - 0.15)
                currentNote_ = best;

            targetNote_ = currentNote_;
            const double corr = ((double) currentNote_ - m) * (double) amount_;
            corrTarget_ = std::max (-kMaxCorrectionSemitones, std::min (kMaxCorrectionSemitones, corr));
        }
        else
        {
            voicedStreak_ = 0;
            if (++unvoicedCount_ >= 5)          // ~20 ms sin voz
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
    int voicedStreak_ = 0, unvoicedCount_ = 100, currentNote_ = -1, targetNote_ = -1;
    float detectedHz_ = 0.0f;
};

} // namespace at
