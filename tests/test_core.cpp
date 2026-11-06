// test_core.cpp - prueba del motor sin JUCE.
// Genera una voz sintética (señal armónica), la pasa por el motor y mide el
// tono de salida con una autocorrelación independiente del detector YIN.
#include "../Source/AutoTuneCore.h"

#include <cstdio>
#include <cstdlib>
#include <random>

static const double kSR = 44100.0;

static std::vector<float> makeVoice (double hz, double seconds)
{
    std::vector<float> v ((size_t) (seconds * kSR));
    double ph = 0.0;
    for (auto& s : v)
    {
        double y = 0.0;
        for (int k = 1; k <= 8; ++k)
            y += std::sin (k * ph) / k;
        s = (float) (0.3 * y);
        ph += 2.0 * 3.14159265358979323846 * hz / kSR;
    }
    return v;
}

// Autocorrelación normalizada: primer máximo local cercano al máximo global
static double measureHz (const std::vector<float>& x, double fromSec, double toSec)
{
    const size_t a = (size_t) (fromSec * kSR), b = (size_t) (toSec * kSR);
    const int minLag = (int) (kSR / 1000.0), maxLag = (int) (kSR / 70.0);
    std::vector<double> ac ((size_t) maxLag + 2, 0.0);
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double num = 0, e1 = 0, e2 = 0;
        for (size_t i = a; i + (size_t) lag < b; ++i)
        {
            num += (double) x[i] * x[i + (size_t) lag];
            e1 += (double) x[i] * x[i];
            e2 += (double) x[i + (size_t) lag] * x[i + (size_t) lag];
        }
        ac[(size_t) lag] = num / std::sqrt (e1 * e2 + 1e-12);
    }
    double best = 0;
    for (int lag = minLag; lag <= maxLag; ++lag)
        best = std::max (best, ac[(size_t) lag]);
    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        if (ac[(size_t) lag] > 0.9 * best && ac[(size_t) lag] >= ac[(size_t) lag - 1] && ac[(size_t) lag] >= ac[(size_t) lag + 1])
        {
            const double s0 = ac[(size_t) lag - 1], s1 = ac[(size_t) lag], s2 = ac[(size_t) lag + 1];
            const double d = s0 - 2 * s1 + s2;
            const double l = lag + (std::abs (d) > 1e-12 ? 0.5 * (s0 - s2) / d : 0.0);
            return kSR / l;
        }
    }
    return 0.0;
}

static double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

static int failures = 0;

static void runCase (const char* name, double inHz, int key, int scale, float speedMs, float amount, double expectHz, double tolCents)
{
    auto voice = makeVoice (inHz, 3.0);
    std::vector<float> left = voice, right = voice;

    at::Engine eng;
    eng.prepare (kSR, 2);
    eng.setParams (key, scale, speedMs, amount);

    const int block = 256;
    for (size_t i = 0; i < voice.size(); i += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, voice.size() - i);
        float* chans[2] = { left.data() + i, right.data() + i };
        eng.process (chans, 2, n);
    }

    const double outHz = measureHz (left, 2.0, 2.9);
    const double err = outHz > 0 ? cents (outHz, expectHz) : 9999.0;
    const bool ok = std::abs (err) <= tolCents;
    if (! ok) ++failures;
    std::printf ("%-44s in %7.2f Hz -> out %7.2f Hz (esperado %7.2f, error %+6.1f cents)  %s\n",
                 name, inHz, outHz, expectHz, err, ok ? "OK" : "FALLO");
}

int main()
{
    // C=0, escalas: 0 cromática, 1 mayor
    runCase ("Cromatica, 450 Hz (A4 +39c), velocidad 0",  450.0, 0, 0, 0.0f, 1.0f, 440.0, 10.0);
    runCase ("Cromatica, 200 Hz (G3 +27c), velocidad 0",  200.0, 0, 0, 0.0f, 1.0f, 196.0, 10.0);
    runCase ("Cromatica, 330 Hz (E4 +4c), velocidad 0",   330.0, 0, 0, 0.0f, 1.0f, 329.63, 10.0);
    runCase ("Cromatica, 120 Hz (grave), velocidad 0",    120.0, 0, 0, 0.0f, 1.0f, 123.47, 12.0);   // B2 = 123.47
    runCase ("Cromatica, 700 Hz (agudo), velocidad 0",    700.0, 0, 0, 0.0f, 1.0f, 698.46, 12.0);   // F5
    runCase ("Do mayor, 455 Hz -> A4",                    455.0, 0, 1, 0.0f, 1.0f, 440.0, 10.0);
    runCase ("Do mayor, 462 Hz (casi A#4) -> A4",         462.0, 0, 1, 0.0f, 1.0f, 440.0, 15.0);
    runCase ("Do mayor, 475 Hz (casi A#4+) -> B4",        475.0, 0, 1, 0.0f, 1.0f, 493.88, 15.0);
    runCase ("Cromatica, 450 Hz, velocidad 50 ms",        450.0, 0, 0, 50.0f, 1.0f, 440.0, 10.0);
    runCase ("Amount 0% (no debe tocar nada)",            450.0, 0, 0, 0.0f, 0.0f, 450.0, 8.0);
    runCase ("Amount 50% (a medio camino)",               450.0, 0, 0, 0.0f, 0.5f, 445.0, 12.0);

    // Ruido sin tono: no debe explotar ni generar valores raros
    {
        std::mt19937 rng (1);
        std::uniform_real_distribution<float> dist (-0.3f, 0.3f);
        std::vector<float> n ((size_t) (2 * kSR));
        for (auto& s : n) s = dist (rng);
        at::Engine eng;
        eng.prepare (kSR, 1);
        eng.setParams (0, 0, 0.0f, 1.0f);
        float peak = 0;
        for (size_t i = 0; i < n.size(); i += 256)
        {
            float* ch[1] = { n.data() + i };
            eng.process (ch, 1, (int) std::min<size_t> (256, n.size() - i));
        }
        for (auto s : n) peak = std::max (peak, std::abs (s));
        const bool ok = peak < 1.0f && std::isfinite (peak);
        if (! ok) ++failures;
        std::printf ("%-44s pico de salida %.3f  %s\n", "Ruido blanco (sin tono)", peak, ok ? "OK" : "FALLO");
    }

    std::printf ("\n%s\n", failures == 0 ? "TODAS LAS PRUEBAS OK" : "HAY FALLOS");
    return failures == 0 ? 0 : 1;
}
