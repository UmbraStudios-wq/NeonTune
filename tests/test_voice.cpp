// test_voice.cpp - pruebas con voz sintetica "dificil".
//   A) Notas con vibrato, deriva, jitter, respiracion y consonantes (voz rica en armonicos)
//   B) Glissandos (el habla y el rap cruzan muchos tonos)
//   C) Ruido de fondo / respiracion sin voz (no debe "inventarse" tonos)
// Mide sobre la SALIDA: error de tono, periodicidad (1.0 = onda limpia) y cresta de la
// segunda diferencia (sube si hay clicks), siempre comparado con la ENTRADA.
#include "../Source/AutoTuneCore.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <random>

static double kSR = 48000.0;
static const double kPi = 3.14159265358979323846;

struct Seg { size_t start, end; double targetHz; };

struct Voice
{
    std::vector<float> x;
    std::vector<Seg> notes, glides;
    std::vector<std::pair<size_t, size_t>> consonants;
};

static double lowpassNoise (std::mt19937& rng, double& state, double coeff)
{
    std::normal_distribution<double> n (0.0, 1.0);
    state += coeff * (n (rng) - state);
    return state;
}

// Una sola voz: f(t) devuelve cents respecto a 'base' en el instante t (s).
static void synthSegment (Voice& v, std::mt19937& rng, double base, size_t len,
                          const std::function<double (double)>& centsAt, bool rich, double breathLevel)
{
    double ph = 0.0, jitterState = 0.0, breathState = 0.0;
    for (size_t i = 0; i < len; ++i)
    {
        const double t = (double) i / kSR;
        const double cents = centsAt (t) + 3.0 * lowpassNoise (rng, jitterState, 0.02);
        const double f = base * std::pow (2.0, cents / 1200.0);
        ph += 2.0 * kPi * f / kSR;

        double y = 0.0;
        for (int k = 1; k <= 60; ++k)
        {
            const double fk = k * f;
            if (fk > 9000.0) break;
            const double form = 1.0 / (1.0 + std::pow ((fk - 750.0) / 350.0, 2.0))
                              + 0.6 / (1.0 + std::pow ((fk - 1250.0) / 450.0, 2.0))
                              + 0.25 / (1.0 + std::pow ((fk - 3100.0) / 700.0, 2.0));
            const double amp = rich ? 1.0 / (double) k : 1.0 / ((double) k * k * 0.25 + 0.75);
            y += form * amp * std::sin (k * ph);
        }
        const double env = std::min (1.0, t / 0.02) * std::min (1.0, ((double) len / kSR - t) / 0.04);
        const double breath = breathLevel * lowpassNoise (rng, breathState, 0.35);
        v.x.push_back ((float) (0.3 * env * y + breath * env));
    }
}

static void addConsonant (Voice& v, std::mt19937& rng, double seconds)
{
    std::normal_distribution<double> gauss (0.0, 1.0);
    const size_t clen = (size_t) (seconds * kSR);
    v.consonants.push_back ({ v.x.size(), v.x.size() + clen });
    double hpPrev = 0;
    for (size_t i = 0; i < clen; ++i)
    {
        const double w = gauss (rng);
        const double hp = w - hpPrev;
        hpPrev = w;
        v.x.push_back ((float) (0.10 * std::sin (kPi * (double) i / (double) clen) * hp));
    }
}

static Voice makeNotes (unsigned seed, bool rich)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> uni (-1.0, 1.0);
    const double noteHz[] = { 220.0, 261.63, 329.63, 293.66, 196.0, 392.0 };
    Voice v;
    for (double base : noteHz)
    {
        const size_t len = (size_t) (0.75 * kSR);
        const double detune = 12.0 * uni (rng), phaseVib = 2.0 * kPi * uni (rng);
        v.notes.push_back ({ v.x.size(), v.x.size() + len, base });
        synthSegment (v, rng, base, len, [&] (double t)
        {
            const double depth = 22.0 * std::min (1.0, std::max (0.0, (t - 0.12) / 0.2));
            return detune + depth * std::sin (2.0 * kPi * 5.5 * t + phaseVib);
        }, rich, 0.02);
        addConsonant (v, rng, 0.11);
    }
    return v;
}

static Voice makeGlides (unsigned seed)
{
    std::mt19937 rng (seed);
    Voice v;
    const double fromTo[][2] = { { 150.0, 300.0 }, { 320.0, 170.0 }, { 130.0, 210.0 } };
    for (auto& ft : fromTo)
    {
        const size_t len = (size_t) (0.7 * kSR);
        v.glides.push_back ({ v.x.size(), v.x.size() + len, 0.0 });
        const double span = 1200.0 * std::log2 (ft[1] / ft[0]);
        synthSegment (v, rng, ft[0], len, [&] (double t)
        {
            const double u = std::min (1.0, std::max (0.0, (t - 0.05) / 0.55));
            return span * (0.5 - 0.5 * std::cos (kPi * u));   // curva suave
        }, true, 0.02);
        addConsonant (v, rng, 0.10);
    }
    return v;
}

static Voice makeNoise (unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> gauss (0.0, 1.0);
    Voice v;
    double lp = 0, breath = 0;
    for (size_t i = 0; i < (size_t) (3.0 * kSR); ++i)
    {
        const double t = (double) i / kSR;
        const double floorNoise = 0.0025 * gauss (rng);                      // ruido de sala (~ -52 dB)
        const double b = (t > 1.0 && t < 1.6) ? 0.05 * lowpassNoise (rng, breath, 0.2) : 0.0;   // respiracion
        lp += 0.05 * (gauss (rng) - lp);
        v.x.push_back ((float) (floorNoise + b + 0.004 * lp));
    }
    return v;
}

// ---------------------------------------------------------------------------
struct PitchMeasure { double hz = 0, peak = 0; };

static PitchMeasure measure (const std::vector<float>& x, size_t a, size_t b)
{
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
    PitchMeasure m;
    for (int lag = minLag + 1; lag < maxLag; ++lag)
        if (ac[(size_t) lag] > 0.9 * best && ac[(size_t) lag] >= ac[(size_t) lag - 1] && ac[(size_t) lag] >= ac[(size_t) lag + 1])
        {
            const double s0 = ac[(size_t) lag - 1], s1 = ac[(size_t) lag], s2 = ac[(size_t) lag + 1];
            const double d = s0 - 2 * s1 + s2;
            m.hz = kSR / (lag + (std::abs (d) > 1e-12 ? 0.5 * (s0 - s2) / d : 0.0));
            m.peak = s1;
            return m;
        }
    return m;
}

static double crest2 (const std::vector<float>& x, size_t a, size_t b)
{
    double sum = 0, peak = 0;
    size_t n = 0;
    for (size_t i = a + 2; i < b; ++i)
    {
        const double d2 = (double) x[i] - 2.0 * x[i - 1] + x[i - 2];
        sum += d2 * d2;
        peak = std::max (peak, std::abs (d2));
        ++n;
    }
    const double rms = std::sqrt (sum / (double) std::max<size_t> (1, n));
    return rms > 1e-12 ? peak / rms : 0.0;
}

static double percentile (std::vector<double> v, double p)
{
    if (v.empty()) return 0;
    std::sort (v.begin(), v.end());
    return v[(size_t) (p * (double) (v.size() - 1))];
}

struct Report
{
    double medErr = 0, p95Err = 0, meanPer = 0, p5Per = 0, minPer = 0, meanCrest = 0, p95Crest = 0, cons = 0;
    double meanHf = 0, p5Hf = 0;      // periodicidad de los AGUDOS (detecta fases rotas entre cabezales)
};

// Analiza ventanas dentro de los segmentos; 'withTarget' calcula error de tono.
static Report analyse (const Voice& v, const std::vector<Seg>& segs, const std::vector<float>& out,
                       size_t lat, bool withTarget, double margin0, double margin1, double winSec)
{
    Report r;
    std::vector<double> errs, pers, crests, hfs;
    const size_t win = (size_t) (winSec * kSR), hop = win / 2;
    std::vector<float> hf (out.size(), 0.0f);          // segunda diferencia = solo agudos
    for (size_t i = 2; i < out.size(); ++i)
        hf[i] = out[i] - 2.0f * out[i - 1] + out[i - 2];
    for (auto& s : segs)
    {
        const size_t a0 = s.start + lat + (size_t) (margin0 * kSR);
        const size_t b0 = s.end + lat - (size_t) (margin1 * kSR);
        for (size_t a = a0; a + win <= b0 && a + win < out.size(); a += hop)
        {
            const auto m = measure (out, a, a + win);
            const auto mh = measure (hf, a, a + win);
            hfs.push_back (mh.hz > 0 ? mh.peak : 0.0);
            pers.push_back (m.hz > 0 ? m.peak : 0.0);
            crests.push_back (crest2 (out, a, a + win));
            if (withTarget)
                errs.push_back (m.hz > 0 ? std::abs (1200.0 * std::log2 (m.hz / s.targetHz)) : 999.0);
        }
    }
    r.medErr = percentile (errs, 0.5);
    r.p95Err = percentile (errs, 0.95);
    double s = 0; for (double p : pers) s += p;
    r.meanPer = pers.empty() ? 0 : s / (double) pers.size();
    r.p5Per = percentile (pers, 0.05);
    r.minPer = pers.empty() ? 0 : *std::min_element (pers.begin(), pers.end());
    double cs = 0; for (double c : crests) cs += c;
    r.meanCrest = crests.empty() ? 0 : cs / (double) crests.size();
    r.p95Crest = percentile (crests, 0.95);
    { double hs = 0; for (double h : hfs) hs += h; r.meanHf = hfs.empty() ? 0 : hs / (double) hfs.size(); }
    r.p5Hf = percentile (hfs, 0.05);

    double eo = 0, ei = 0;
    for (auto& c : v.consonants)
        for (size_t i = c.first + 1500; i + 1500 < c.second; ++i)
        {
            ei += (double) v.x[i] * v.x[i];
            if (i + lat < out.size()) eo += (double) out[i + lat] * out[i + lat];
        }
    r.cons = ei > 0 ? std::sqrt (eo / ei) : 0;
    return r;
}

static void print (const char* name, const Report& r, bool withTarget)
{
    if (withTarget)
        std::printf ("  %-26s error med %5.1f p95 %5.1f c | period. %.3f (p5 %.3f) | cresta %4.1f/%4.1f | consonantes x%.2f\n",
                     name, r.medErr, r.p95Err, r.meanPer, r.p5Per, r.meanCrest, r.p95Crest, r.cons);
    else
        std::printf ("  %-26s                          | period. %.3f (p5 %.3f) | cresta %4.1f/%4.1f | consonantes x%.2f\n",
                     name, r.meanPer, r.p5Per, r.meanCrest, r.p95Crest, r.cons);
}

static std::vector<float> run (const std::vector<float>& in, float speedMs, int block, int scale,
                               int* latency, double* voicedFrac = nullptr)
{
    std::vector<float> out = in;
    at::Engine eng;
    eng.prepare (kSR, 1);
    eng.setParams (0, scale, speedMs, 1.0f);
    int blocks = 0, voiced = 0;
    for (size_t i = 0; i < out.size(); i += (size_t) block)
    {
        float* ch[1] = { out.data() + i };
        eng.process (ch, 1, (int) std::min<size_t> ((size_t) block, out.size() - i));
        ++blocks;
        if (eng.detectedHz() > 0) ++voiced;
    }
    *latency = eng.latencySamples();
    if (voicedFrac) *voicedFrac = (double) voiced / (double) std::max (1, blocks);
    return out;
}

int main (int argc, char** argv)
{
    const int block = argc > 1 ? std::atoi (argv[1]) : 256;
    if (argc > 2) kSR = std::atof (argv[2]);
    std::printf ("Bloque %d muestras, %.0f Hz\n", block, kSR);
    int lat = 0;

    for (int rich = 0; rich <= 1; ++rich)
    {
        const Voice v = makeNotes (7, rich == 1);
        std::printf ("\nA) NOTAS con vibrato + consonantes (%s)\n", rich ? "voz RICA en armonicos" : "voz suave");
        print ("entrada", analyse (v, v.notes, v.x, 0, true, 0.16, 0.05, 0.040), true);
        for (float sp : { 0.0f, 30.0f, 90.0f })
        {
            auto out = run (v.x, sp, block, 0, &lat);
            char nm[48]; std::snprintf (nm, sizeof nm, "salida, velocidad %g ms", sp);
            print (nm, analyse (v, v.notes, out, (size_t) lat, true, 0.16, 0.05, 0.040), true);
        }
    }

    {
        const Voice v = makeGlides (11);
        std::printf ("\nB) GLISSANDOS (150->300, 320->170, 130->210 Hz), escala cromatica\n");
        print ("entrada", analyse (v, v.glides, v.x, 0, false, 0.06, 0.08, 0.030), false);
        for (float sp : { 0.0f, 30.0f, 90.0f })
        {
            auto out = run (v.x, sp, block, 0, &lat);
            char nm[48]; std::snprintf (nm, sizeof nm, "salida, velocidad %g ms", sp);
            print (nm, analyse (v, v.glides, out, (size_t) lat, false, 0.06, 0.08, 0.030), false);
        }
        std::printf ("  (escala Do mayor, velocidad 30 ms)\n");
        auto out = run (v.x, 30.0f, block, 1, &lat);
        print ("salida Do mayor", analyse (v, v.glides, out, (size_t) lat, false, 0.06, 0.08, 0.030), false);
    }

    {
        const Voice v = makeNoise (3);
        double frac = 0;
        auto out = run (v.x, 0.0f, block, 0, &lat, &frac);
        double ei = 0, eo = 0, maxd = 0;
        for (size_t i = 0; i + (size_t) lat < out.size(); ++i)
        {
            ei += (double) v.x[i] * v.x[i];
            eo += (double) out[i + (size_t) lat] * out[i + (size_t) lat];
        }
        // salto maximo entre muestras vs. la entrada
        double maxIn = 0;
        for (size_t i = 1; i < out.size(); ++i)
        {
            maxd = std::max (maxd, (double) std::abs (out[i] - out[i - 1]));
            maxIn = std::max (maxIn, (double) std::abs (v.x[i] - v.x[i - 1]));
        }
        std::printf ("\nC) RUIDO de sala + respiracion (sin voz)\n  bloques con tono detectado: %.1f %% (ideal 0)   nivel salida/entrada x%.2f   salto max salida/entrada x%.2f\n",
                     100.0 * frac, std::sqrt (eo / ei), maxd / maxIn);
    }
    return 0;
}
