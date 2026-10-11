// Tests hors-ligne du moteur (aucune dépendance) :
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I Source Tests/EngineTests.cpp -o engine_tests && ./engine_tests
//
// Les bandes sont remplies de NaN à chaque redémarrage (PHIBRES_POISON_TAPES) : un grain qui lirait
// un endroit jamais enregistré contaminerait la sortie, ce que les tests verraient aussitôt.

#define PHIBRES_POISON_TAPES 1

#include "GrainStretcher.h"
#include "OutputFilter.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <utility>

static int failures = 0;

#define CHECK(cond, ...) \
    do { if (! (cond)) { ++failures; std::printf ("  ECHEC ligne %d : ", __LINE__); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (false)

static const double kSampleRate = 48000.0;
static const double kPi = 3.14159265358979323846;

using Signal = std::vector<float>;
using Action = std::function<void (GrainStretcher&)>;
using Events = std::vector<std::pair<int, Action>>;

static Signal makeNoise (int n, unsigned seed, float amp = 0.5f)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-amp, amp);
    Signal v ((size_t) n);
    for (auto& x : v) x = dist (rng);
    return v;
}

static Signal makeSine (int n, double freq, float amp, double sampleRate = kSampleRate)
{
    Signal v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = amp * (float) std::sin (2.0 * kPi * freq * i / sampleRate);
    return v;
}

/** Fait tourner le moteur en mono. Les évènements sont appliqués à l'échantillon près. */
static Signal run (GrainStretcher& engine, const Signal& in, Events events = {}, int blockSize = 512)
{
    Signal out = in;
    size_t next = 0;
    const int total = (int) in.size();

    for (int start = 0; start < total;)
    {
        while (next < events.size() && events[next].first <= start)
            events[next++].second (engine);

        int n = std::min (blockSize, total - start);

        if (next < events.size())
            n = std::min (n, events[next].first - start);

        float* channel[1] = { out.data() + start };
        engine.process (channel, 1, n);
        start += n;
    }

    return out;
}

static void runStereo (GrainStretcher& engine, Signal& left, Signal& right, Events events = {}, int blockSize = 512)
{
    size_t next = 0;
    const int total = (int) left.size();

    for (int start = 0; start < total;)
    {
        while (next < events.size() && events[next].first <= start)
            events[next++].second (engine);

        int n = std::min (blockSize, total - start);

        if (next < events.size())
            n = std::min (n, events[next].first - start);

        float* channel[2] = { left.data() + start, right.data() + start };
        engine.process (channel, 2, n);
        start += n;
    }
}

static bool allFinite (const Signal& v)
{
    for (const float x : v)
        if (! std::isfinite (x))
            return false;

    return true;
}

static float peakOf (const Signal& v, int from, int to)
{
    float m = 0.0f;
    for (int i = std::max (0, from); i < std::min (to, (int) v.size()); ++i)
        m = std::max (m, std::abs (v[(size_t) i]));
    return m;
}

static double rmsOf (const Signal& v, int from, int to)
{
    double sum = 0.0;
    int count = 0;
    for (int i = std::max (0, from); i < std::min (to, (int) v.size()); ++i, ++count)
        sum += (double) v[(size_t) i] * (double) v[(size_t) i];
    return count > 0 ? std::sqrt (sum / count) : 0.0;
}

static float maxStep (const Signal& v, int from, int to)
{
    float m = 0.0f;
    for (int i = std::max (1, from); i < std::min (to, (int) v.size()); ++i)
        m = std::max (m, std::abs (v[(size_t) i] - v[(size_t) (i - 1)]));
    return m;
}

static double decibels (double ratio)     { return 20.0 * std::log10 (std::max (1.0e-12, ratio)); }

/** Niveau d'une fréquence dans une tranche du signal (fenêtre de Hann). */
static double levelAt (const Signal& v, int from, int to, double freq, double sampleRate = kSampleRate)
{
    double re = 0.0, im = 0.0;
    const int n = to - from;

    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * i / n);
        const double x = w * (double) v[(size_t) (from + i)];
        re += x * std::cos (2.0 * kPi * freq * i / sampleRate);
        im += x * std::sin (2.0 * kPi * freq * i / sampleRate);
    }

    return std::sqrt (re * re + im * im) / n;
}

/** Fréquence la plus forte dans une tranche du signal. */
static double dominantFrequency (const Signal& v, int from, int to, double lowest, double highest, double sampleRate = kSampleRate)
{
    double best = lowest, bestLevel = -1.0;

    for (double f = lowest; f <= highest; f += 4.0)
    {
        const double level = levelAt (v, from, to, f, sampleRate);
        if (level > bestLevel) { bestLevel = level; best = f; }
    }

    const double centre = best;

    for (double f = centre - 4.0; f <= centre + 4.0; f += 0.25)
    {
        const double level = levelAt (v, from, to, f, sampleRate);
        if (level > bestLevel) { bestLevel = level; best = f; }
    }

    return best;
}

/** Réglages de base : un nuage lisse (fenêtre de Hann, recouvrement de moitié), sans dispersion. */
static void configure (GrainStretcher& e, double sampleRate = kSampleRate, int channels = 1, double maxSeconds = 30.0)
{
    e.prepare (sampleRate, channels, maxSeconds);
    e.setLoopLength (sampleRate);
    e.setAutoCapture (false, sampleRate);
    e.setSpeed (1.0);
    e.setFrozen (false);
    e.setBackward (false);
    e.setPosition (0.0);
    e.setGrainPeriod (1024.0);
    e.setGrainSize (2.0);
    e.setShape (1.0);
    e.setTilt (0.0);
    e.setPitch (0.0);
    e.setFlip (false);
    e.setScatter (false, 0.0, 0.0, 0, 0.0, 0.0);
    e.setActive (false);
}

static void checkHealthy (const GrainStretcher& e, const char* what)
{
    CHECK (e.getOutOfRangeReads() == 0, "%s : %d lectures hors de la zone enregistree", what, e.getOutOfRangeReads());
    CHECK (e.getStolenGrains() == 0, "%s : %d grains coupes par un changement de bande", what, e.getStolenGrains());
}

//==============================================================================
static void testPassthrough()
{
    std::printf ("1. Eteint : le signal passe intact\n");
    GrainStretcher e;
    configure (e);

    const auto in = makeNoise (40000, 1);
    const auto out = run (e, in, {}, 333);

    bool same = true;
    for (size_t i = 0; i < in.size(); ++i) same = same && in[i] == out[i];
    CHECK (same, "la sortie devrait etre identique a l'entree");
    CHECK (! e.isRunning(), "le moteur ne devrait pas tourner");
}

static void testUnityIsTransparent()
{
    std::printf ("2. Vitesse 1, sans transposition : transparent au premier passage, puis boucle exacte\n");
    const int S = 5000, L = 24000, N = 100000, grain = 2048;

    for (const double period : { 1024.0, 1000.37 })
    {
        GrainStretcher e;
        configure (e);
        e.setLoopLength ((double) L);
        e.setGrainPeriod (period);

        const auto in = makeNoise (N, 2);
        const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);
        const float tolerance = period == 1024.0 ? 2.0e-4f : 6.0e-3f;

        bool before = true;
        float firstPass = 0.0f, loop = 0.0f;

        for (int t = 0; t < S; ++t)
            before = before && out[(size_t) t] == in[(size_t) t];

        for (int t = S; t < S + L; ++t)
            firstPass = std::max (firstPass, std::abs (out[(size_t) t] - in[(size_t) t]));

        // Après le premier passage : la capture rejouée en boucle. Juste après chaque raccord,
        // les grains d'avant et d'après se croisent pendant la durée d'un grain : on saute cette zone.
        for (int t = S + L; t < N; ++t)
        {
            const int k = (t - S) % L;

            if (k >= grain + 16)
                loop = std::max (loop, std::abs (out[(size_t) t] - in[(size_t) (S + k)]));
        }

        CHECK (before, "audio modifie avant l'activation");
        CHECK (firstPass < tolerance, "periode %.2f : ecart au premier passage %g (max %g)", period, (double) firstPass, (double) tolerance);
        CHECK (loop < tolerance, "periode %.2f : ecart en boucle %g (max %g)", period, (double) loop, (double) tolerance);
        CHECK (allFinite (out), "sortie non finie");
        checkHealthy (e, "vitesse 1");
    }
}

static void testAddressing()
{
    std::printf ("3. Position, sens et hauteur des grains : lecture exacte (tete gelee)\n");
    const int S = 3000, L = 20000, N = 60000, P = 1024, edge = 48;   // bords de 1 ms a 48 kHz
    const int at = L / 2;                                             // position 50 %

    struct Case { const char* name; double pitch; bool flip; };

    for (const auto c : { Case { "normal", 0.0, false }, Case { "a l'envers", 0.0, true },
                          Case { "+12", 12.0, false }, Case { "-12", -12.0, false }, Case { "+12 a l'envers", 12.0, true } })
    {
        GrainStretcher e;
        configure (e);
        e.setLoopLength ((double) L);
        e.setFrozen (true);
        e.setPosition (0.5);
        e.setGrainPeriod ((double) P);
        e.setGrainSize (1.0);
        e.setShape (0.0);
        e.setPitch (c.pitch);
        e.setFlip (c.flip);

        const auto in = makeNoise (N, 3);
        const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 480);
        const double rate = std::exp2 (c.pitch / 12.0);

        float worst = 0.0f;
        int checked = 0;

        // Les grains partent toutes les P échantillons depuis S. On attend que la zone lue soit enregistrée.
        for (int t0 = S + 40 * P; t0 + P < N; t0 += P)
        {
            for (int k = edge + 2; k < P - edge - 2; ++k)
            {
                const double source = c.flip ? (double) at + (P - 1 - k) * rate : (double) at + k * rate;

                if (source != std::floor (source))
                    continue;   // position entre deux échantillons : interpolée, on ne compare pas

                worst = std::max (worst, std::abs (out[(size_t) (t0 + k)] - in[(size_t) (S + (int) source)]));
                ++checked;
            }
        }

        CHECK (checked > 1000, "%s : trop peu de points compares (%d)", c.name, checked);
        CHECK (worst == 0.0f, "%s : ecart de lecture %g", c.name, (double) worst);
        CHECK (std::abs (e.getTravel()) < 1.0e-9, "%s : la tete a bouge (%g)", c.name, e.getTravel());
        checkHealthy (e, c.name);
    }
}

static void testStretchKeepsPitch()
{
    std::printf ("4. Etirement x4 : chaque passage dure quatre fois plus longtemps, a la meme hauteur\n");
    const int S = 4800, segment = 4800, numSegments = 10, L = segment * numSegments;
    const int N = S + 4 * L + 4800;

    // Dix notes de 100 ms, de 400 à 2200 Hz.
    Signal in ((size_t) N, 0.0f);
    for (int i = 0; i < N - S; ++i)
    {
        const int k = (i / segment) % numSegments;
        in[(size_t) (S + i)] = 0.5f * (float) std::sin (2.0 * kPi * (400.0 + 200.0 * k) * i / kSampleRate);
    }

    GrainStretcher e;
    configure (e);
    e.setLoopLength ((double) L);
    e.setSpeed (0.25);
    e.setGrainPeriod (960.0);   // 20 ms : les partiels tombent sur une grille de 50 Hz

    const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);

    for (int k = 0; k < numSegments; ++k)
    {
        const int centre = S + 4 * segment * k + 2 * segment;
        const double expected = 400.0 + 200.0 * k;
        const double found = dominantFrequency (out, centre - 2400, centre + 2400, 200.0, 2600.0);
        CHECK (std::abs (found - expected) <= 30.0, "note %d : %.1f Hz au lieu de %.1f", k, found, expected);
    }

    const double expectedTravel = std::fmod (0.25 * (double) (N - S), (double) L);
    CHECK (std::abs (e.getTravel() - expectedTravel) < 1.0e-6, "tete a %.1f au lieu de %.1f", e.getTravel(), expectedTravel);
    CHECK (rmsOf (out, S + L, S + 3 * L) > 0.15, "niveau trop faible : %g", rmsOf (out, S + L, S + 3 * L));
    checkHealthy (e, "etirement");
}

static void testPitch()
{
    std::printf ("5. Transposition : la hauteur change, pas la vitesse de la tete\n");
    const int S = 48000, N = 3 * 48000;

    for (const double semitones : { 12.0, -12.0, 7.0, 24.0 })
    {
        GrainStretcher e;
        configure (e);
        e.setLoopLength (48000.0);
        e.setSpeed (0.5);
        e.setGrainPeriod (480.0);   // 10 ms : partiels sur une grille de 100 Hz, décalée de 100 Hz (200 Hz x 0,5)
        e.setGrainSize (4.0);
        e.setPitch (semitones);

        const auto in = makeSine (N, 200.0, 0.5f);
        const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);
        const double expected = 200.0 * std::exp2 (semitones / 12.0);
        const double found = dominantFrequency (out, S + 24000, S + 72000, 60.0, 1200.0);

        CHECK (std::abs (found - expected) <= 55.0, "%+.0f demi-tons : %.1f Hz au lieu de %.1f", semitones, found, expected);
        CHECK (std::abs (e.getTravel() - std::fmod (0.5 * (double) (N - S), 48000.0)) < 2.0, "%+.0f demi-tons : tete a %.1f", semitones, e.getTravel());
        CHECK (allFinite (out), "sortie non finie");
        checkHealthy (e, "transposition");
    }
}

static void testPlayhead()
{
    std::printf ("6. Tete de lecture : vitesse, gel, marche arriere, bouclage\n");
    GrainStretcher e;
    configure (e);
    e.setLoopLength (10000.0);
    e.setSpeed (0.25);
    e.setActive (true);

    Signal buffer = makeNoise (20000, 4);
    float* channel[1] = { buffer.data() };

    e.process (channel, 1, 20000);
    CHECK (std::abs (e.getTravel() - 5000.0) < 1.0e-6, "vitesse 0,25 : tete a %g au lieu de 5000", e.getTravel());

    // Le gel s'installe en douceur : constante de temps de 10 ms, soit un arrêt à 0,25 x 480 échantillons plus loin.
    e.setFrozen (true);
    e.process (channel, 1, 2400);
    CHECK (std::abs (e.getTravel() - 5120.0) < 2.0, "gel : apres 50 ms la tete est a %g au lieu de 5120 environ", e.getTravel());
    e.process (channel, 1, 12000);
    const double frozenAt = e.getTravel();
    e.process (channel, 1, 10000);
    CHECK (e.getTravel() == frozenAt, "gel : la tete avance encore (%.9f -> %.9f)", frozenAt, e.getTravel());

    e.setFrozen (false);
    e.setBackward (true);
    e.process (channel, 1, 14400);
    const double turned = e.getTravel();
    e.process (channel, 1, 8000);
    const double wentBack = std::fmod (turned - e.getTravel() + 10000.0, 10000.0);   // la tête a pu repasser par le début de la boucle
    CHECK (std::abs (wentBack - 2000.0) < 1.0e-6, "marche arriere : recul de %.9f au lieu de 2000", wentBack);

    // Bouclage : la tête reste dans [0, longueur).
    e.setBackward (false);
    e.setSpeed (1.0);
    bool inside = true;

    for (int i = 0; i < 40; ++i)
    {
        e.process (channel, 1, 997);
        inside = inside && e.getTravel() >= 0.0 && e.getTravel() < 10000.0;
    }

    CHECK (inside, "la tete est sortie de la boucle");

    GrainStretcher::View view;
    CHECK (e.getView (view) && view.running && view.loopLength == 10000.0 && view.playhead >= 0.0 && view.playhead < 1.0, "vue incoherente");
    checkHealthy (e, "tete de lecture");
}

static void testFirstPassNeverReadsAhead()
{
    std::printf ("7. Premier passage : aucun grain ne lit plus loin que l'enregistrement\n");

    struct Case { const char* name; double pitch; bool flip; double speed; bool backward; int warmup; };

    for (const auto c : { Case { "+24 des le depart", 24.0, false, 1.0, false, 60000 },
                          Case { "+12 sans historique", 12.0, false, 1.0, false, 0 },
                          Case { "a l'envers sans historique", 0.0, true, 0.5, false, 0 },
                          Case { "marche arriere", 5.0, false, 1.0, true, 20000 },
                          Case { "-24 a l'envers", -24.0, true, 0.25, false, 300 },
                          Case { "vitesse 4", 0.0, false, 4.0, false, 9000 } })
    {
        GrainStretcher e;
        configure (e);
        e.setLoopLength (30000.0);
        e.setSpeed (c.speed);
        e.setBackward (c.backward);
        e.setPitch (c.pitch);
        e.setFlip (c.flip);
        e.setGrainPeriod (700.0);
        e.setGrainSize (3.0);
        e.setScatter (true, 5000.0, 3.0, 0, 0.3, 1.0);

        const int N = c.warmup + 120000;
        const auto in = makeNoise (N, 5);
        const auto out = run (e, in, { { c.warmup, [] (GrainStretcher& g) { g.setActive (true); } } }, 256);

        CHECK (allFinite (out), "%s : la sortie contient des valeurs non finies (lecture d'une zone jamais enregistree)", c.name);
        CHECK (peakOf (out, 0, N) < 4.0f, "%s : crete %g", c.name, (double) peakOf (out, 0, N));
        checkHealthy (e, c.name);

        if (c.warmup >= 20000)   // avec de l'historique, le son est là dès les premières millisecondes
            CHECK (rmsOf (out, c.warmup + 240, c.warmup + 2400) > 0.05, "%s : trou a l'allumage (%g)", c.name, rmsOf (out, c.warmup + 240, c.warmup + 2400));
    }
}

static void testAutoCapture()
{
    std::printf ("8. Recapture periodique et manuelle\n");
    GrainStretcher e;
    configure (e);
    e.setLoopLength (6000.0);
    e.setSpeed (0.25);
    e.setAutoCapture (true, 10000.5);
    e.setActive (true);

    const int S = 100, N = 120000;
    Signal buffer = makeNoise (N, 6);
    std::vector<int> times;
    unsigned int last = 0;

    for (int t = 0; t < N; ++t)
    {
        if (t == S) e.setActive (true);
        float* channel[1] = { buffer.data() + t };
        e.process (channel, 1, 1);

        if (e.getCaptureCount() != last)
        {
            last = e.getCaptureCount();
            times.push_back (t);
        }
    }

    CHECK (times.size() == 12, "%d captures au lieu de 12", (int) times.size());

    bool regular = true;
    for (size_t i = 1; i < times.size(); ++i)
        regular = regular && (times[i] - times[i - 1] == 10000 || times[i] - times[i - 1] == 10001);

    CHECK (regular, "intervalles irreguliers");

    if (times.size() > 2)
    {
        const double mean = (double) (times.back() - times.front()) / (double) (times.size() - 1);
        CHECK (std::abs (mean - 10000.5) < 0.2, "intervalle moyen %.3f au lieu de 10000.5", mean);
    }

    CHECK (e.getTravel() < 0.25 * 10001.0, "la tete ne repart pas de zero a chaque capture (%g)", e.getTravel());

    // Recapture manuelle : elle repart du son présent et remet le compteur à zéro.
    GrainStretcher m;
    configure (m);
    m.setLoopLength (6000.0);
    m.setFrozen (true);
    m.setGrainPeriod (1024.0);
    m.setGrainSize (1.0);
    m.setShape (0.0);

    const int T = 30000;
    const auto in = makeNoise (60000, 7);
    const auto out = run (m, in, { { 2000, [] (GrainStretcher& g) { g.setActive (true); } },
                                   { T, [] (GrainStretcher& g) { g.trigger(); } } }, 512);

    // La période tombe juste (1024) : un grain part à T + j * 1024 - ((T - 2000) % 1024)... on retrouve son départ par le contenu.
    const int firstGrain = 2000 + ((T - 2000) / 1024 + 4) * 1024;
    float worst = 0.0f;
    for (int k = 60; k < 1024 - 60; ++k)
        worst = std::max (worst, std::abs (out[(size_t) (firstGrain + k)] - in[(size_t) (T + k)]));

    CHECK (worst == 0.0f, "apres une recapture manuelle, les grains ne lisent pas le debut de la nouvelle capture (ecart %g)", (double) worst);
    CHECK (m.getCaptureCount() == 2, "%u captures au lieu de 2", m.getCaptureCount());
    checkHealthy (e, "recapture periodique");
    checkHealthy (m, "recapture manuelle");
}

static void testTapeSwitching()
{
    std::printf ("9. Longues sessions : changements de bande sans grain coupe\n");
    const double rate = 8000.0;   // bas, pour faire défiler des minutes rapidement

    struct Case { const char* name; double loopSeconds; int everySamples; double period; double size; double pitch; };

    for (const auto c : { Case { "boucle de 30 s, capture toutes les 2,3 s", 30.0, 18400, 4000.0, 8.0, 24.0 },
                          Case { "boucle de 1 s, capture toutes les 5 ms", 1.0, 40, 4000.0, 8.0, -12.0 },
                          Case { "boucle de 12 s, capture toutes les 0,7 s", 12.0, 5600, 900.0, 4.0, 0.0 } })
    {
        GrainStretcher e;
        configure (e, rate, 1, 30.0);
        e.setLoopLength (c.loopSeconds * rate);
        e.setSpeed (0.3);
        e.setGrainPeriod (c.period);
        e.setGrainSize (c.size);
        e.setPitch (c.pitch);
        e.setScatter (true, 4000.0, 2.0, 1, 0.5, 0.0);
        e.setActive (true);

        const int N = (int) (150.0 * rate);
        Signal buffer = makeNoise (N, 8);
        int switches = 0, lastTape = -1, maxGrains = 0;

        for (int start = 0; start < N; start += c.everySamples)
        {
            const int n = std::min (c.everySamples, N - start);
            float* channel[1] = { buffer.data() + start };
            e.trigger();
            e.process (channel, 1, n);
            maxGrains = std::max (maxGrains, e.getNumActiveGrains());

            if (e.getCurrentTape() != lastTape)
            {
                ++switches;
                lastTape = e.getCurrentTape();
            }
        }

        CHECK (allFinite (buffer), "%s : sortie non finie", c.name);
        CHECK (switches >= 3, "%s : la bande n'a change que %d fois", c.name, switches);
        CHECK (maxGrains <= GrainStretcher::maxGrains && maxGrains >= 2, "%s : %d grains", c.name, maxGrains);
        CHECK (rmsOf (buffer, N / 2, N) > 0.02, "%s : sortie trop faible (%g)", c.name, rmsOf (buffer, N / 2, N));
        checkHealthy (e, c.name);
    }
}

static void testLevels()
{
    std::printf ("10. Niveau : conforme au calcul pour un bruit, jamais au-dessus de l'entree pour un son pur\n");
    const int S = 24000, N = 6 * 48000;

    for (const double size : { 0.5, 1.0, 2.0, 4.0, 8.0 })
    {
        for (const double shape : { 0.0, 0.5, 1.0 })
        {
            GrainStretcher e;
            configure (e);
            e.setLoopLength (96000.0);
            e.setSpeed (0.125);
            e.setGrainPeriod (1024.0);
            e.setGrainSize (size);
            e.setShape (shape);

            const auto in = makeNoise (N, 9);
            const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);

            // Grains décorrélés : les puissances s'ajoutent, puis le moteur divise par la somme des fenêtres quand elle dépasse 1.
            // On refait ce calcul sur une période, avec la fenêtre du moteur (bords d'au moins 1 ms, soit 48 échantillons).
            const int length = (int) std::llround (1024.0 * size);
            double attackFraction = 0.0, releaseFraction = 0.0;
            GrainStretcher::windowFractions (shape, 0.0, attackFraction, releaseFraction);
            const double attack = std::max (48.0, attackFraction * length), release = std::max (48.0, releaseFraction * length);
            const auto window = [=] (int i)
            {
                if (i < 0 || i >= length) return 0.0;
                if (i < attack) return 0.5 - 0.5 * std::cos (kPi * i / attack);
                return length - i <= release ? 0.5 - 0.5 * std::cos (kPi * (length - i) / release) : 1.0;
            };

            double power = 0.0;
            for (int t = 0; t < 1024; ++t)
            {
                double sum = 0.0, squares = 0.0;
                for (int grain = 0; grain < 9; ++grain)
                {
                    const double w = window (t + 1024 * grain);
                    sum += w;
                    squares += w * w;
                }
                power += squares / std::pow (std::max (1.0, sum), 2.0) / 1024.0;
            }

            const double expected = std::sqrt (power);
            const double measured = rmsOf (out, S + 48000, N) / rmsOf (in, S + 48000, N);

            CHECK (std::abs (decibels (measured / expected)) < 1.0, "taille %.1f forme %.1f : niveau %.2f dB, attendu %.2f dB",
                   size, shape, decibels (measured), decibels (expected));
            checkHealthy (e, "niveau");
        }
    }

    // Son pur : quels que soient le nombre de grains superposés et leur forme, y compris pendant qu'on change
    // la cadence et la taille, la sortie ne dépasse pas l'entrée.
    struct Window { double shape, tilt; };

    for (const double size : { 0.7, 1.5, 2.0, 3.0, 4.0, 8.0 })
    {
        for (const auto w : { Window { 1.0, 0.0 }, Window { 0.0, 0.0 }, Window { 0.5, -0.7 }, Window { 0.8, 1.0 } })
        {
            GrainStretcher e;
            configure (e);
            e.setLoopLength (96000.0);
            e.setSpeed (0.125);
            e.setGrainPeriod (1024.0);
            e.setGrainSize (size);
            e.setShape (w.shape);
            e.setTilt (w.tilt);

            const auto in = makeSine (N, 110.0, 0.5f);
            const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } },
                                           { S + 96000, [] (GrainStretcher& g) { g.setGrainPeriod (300.0); } },
                                           { S + 120000, [] (GrainStretcher& g) { g.setGrainSize (8.0); g.setShape (0.2); } },
                                           { S + 144000, [] (GrainStretcher& g) { g.setGrainPeriod (5000.0); g.setGrainSize (1.0); } } }, 512);

            CHECK (peakOf (out, S, N) <= 0.5f * 1.01f, "son pur, taille %.1f, forme %.1f, bascule %.1f : crete %g pour une entree a 0.5",
                   size, w.shape, w.tilt, (double) peakOf (out, S, N));
        }
    }
}

static void testNoClicks()
{
    std::printf ("11. Pas de clic : allumage, recaptures, raccords de boucle, gel, marche arriere, extinction\n");
    const int N = 10 * 48000;
    const auto in = makeSine (N, 55.0, 0.8f);
    const float sineStep = 0.8f * (float) (2.0 * kPi * 55.0 / kSampleRate);

    GrainStretcher e;
    configure (e);
    e.setLoopLength (9000.0);         // pas un multiple de la période du sinus
    e.setSpeed (0.37);
    e.setGrainPeriod (1440.0);        // 30 ms
    e.setAutoCapture (false, 48000.0);

    const auto out = run (e, in, {
        { 7001,   [] (GrainStretcher& g) { g.setActive (true); } },
        { 40011,  [] (GrainStretcher& g) { g.trigger(); } },
        { 61003,  [] (GrainStretcher& g) { g.setPosition (0.7); } },
        { 90007,  [] (GrainStretcher& g) { g.setFrozen (true); } },
        { 120001, [] (GrainStretcher& g) { g.setFrozen (false); g.setBackward (true); } },
        { 150003, [] (GrainStretcher& g) { g.setBackward (false); g.setSpeed (1.0); } },
        { 200009, [] (GrainStretcher& g) { g.setAutoCapture (true, 5003.0); } },
        { 260001, [] (GrainStretcher& g) { g.setAutoCapture (false, 5003.0); g.setLoopLength (30001.0); g.setSpeed (0.05); } },
        { 300007, [] (GrainStretcher& g) { g.setActive (false); } },
        { 300107, [] (GrainStretcher& g) { g.setActive (true); } },     // rallumé pendant le fondu de sortie
        { 340001, [] (GrainStretcher& g) { g.setGrainSize (4.0); } },
        { 380003, [] (GrainStretcher& g) { g.setGrainPeriod (333.0); } },
        { 420001, [] (GrainStretcher& g) { g.setActive (false); } },
        { 450003, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);

    const float step = maxStep (out, 0, N);
    CHECK (step < 3.2f * sineStep, "saut de %g (un sinus seul : %g)", (double) step, (double) sineStep);
    CHECK (peakOf (out, 0, N) <= 0.8f * 1.05f, "crete %g", (double) peakOf (out, 0, N));

    bool dry = true;
    for (int t = 420001 + 300; t < 450003; ++t)   // 5 ms de fondu, puis le son direct, intact
        dry = dry && out[(size_t) t] == in[(size_t) t];

    CHECK (dry, "eteint, la sortie n'est pas l'entree");
    checkHealthy (e, "clics");
}

static void testStereoWidth()
{
    std::printf ("12. Largeur stereo : aucune a 0, les grains se repartissent a 100 %%\n");
    const int S = 24000, N = 4 * 48000;

    for (const double width : { 0.0, 1.0 })
    {
        GrainStretcher e;
        configure (e, kSampleRate, 2);
        e.setLoopLength (48000.0);
        e.setSpeed (0.25);
        e.setScatter (true, 0.0, 0.0, 0, 0.0, width);

        auto left = makeNoise (N, 10), right = left;
        const auto in = left;
        runStereo (e, left, right, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);

        double difference = 0.0;
        for (int t = S; t < N; ++t)
            difference = std::max (difference, (double) std::abs (left[(size_t) t] - right[(size_t) t]));

        const double l = rmsOf (left, S + 24000, N), r = rmsOf (right, S + 24000, N), reference = rmsOf (in, S + 24000, N);

        if (width == 0.0)
        {
            CHECK (difference == 0.0, "largeur 0 : les canaux different de %g", difference);
        }
        else
        {
            CHECK (difference > 0.1, "largeur 100 %% : les canaux sont identiques");
            CHECK (std::abs (decibels (l / r)) < 1.5, "largeur 100 %% : desequilibre de %.2f dB", decibels (l / r));
            // Fenêtres de Hann décorrélées : puissance x 0,75. Panoramique tiré au sort : chaque canal garde 0,75 de la sienne en moyenne.
            CHECK (std::abs (decibels (std::sqrt (0.5 * (l * l + r * r)) / (reference * 0.75))) < 1.0,
                   "largeur 100 %% : la puissance totale s'ecarte de %.2f dB du calcul", decibels (std::sqrt (0.5 * (l * l + r * r)) / (reference * 0.75)));
            CHECK (std::max (peakOf (left, S, N), peakOf (right, S, N)) <= peakOf (in, 0, N), "largeur 100 %% : un canal depasse la crete de l'entree");
        }

        CHECK (allFinite (left) && allFinite (right), "sortie non finie");
        checkHealthy (e, "largeur");
    }
}

static void testScatter()
{
    std::printf ("13. Dispersion : position, hauteur (libre, demi-tons, octaves), sens\n");
    const int S = 48000, N = 5 * 48000;

    // Écart de hauteur par octaves : un son pur à 400 Hz ne peut donner que 100, 200, 400, 800 ou 1600 Hz.
    {
        GrainStretcher e;
        configure (e);
        e.setLoopLength (48000.0);
        e.setSpeed (0.25);
        e.setGrainPeriod (2400.0);
        e.setGrainSize (4.0);
        e.setScatter (true, 0.0, 24.0, GrainStretcher::detuneOctaves, 0.0, 0.0);

        const auto in = makeSine (N, 400.0, 0.5f);
        const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);

        double onOctaves = 0.0, between = 0.0;
        for (const double f : { 100.0, 200.0, 400.0, 800.0, 1600.0 })
            onOctaves += levelAt (out, S + 48000, N, f);
        for (const double f : { 150.0, 300.0, 600.0, 1200.0 })
            between += levelAt (out, S + 48000, N, f);

        CHECK (levelAt (out, S + 48000, N, 800.0) > 0.01 && levelAt (out, S + 48000, N, 200.0) > 0.01, "pas d'octave au-dessus / au-dessous");
        CHECK (onOctaves > 20.0 * between, "de l'energie entre les octaves (%g contre %g)", between, onOctaves);
        checkHealthy (e, "octaves");
    }

    // Sans dispersion le moteur est déterministe ; avec, deux réglages différents donnent deux sorties différentes,
    // et l'interrupteur général ramène exactement au son sans dispersion.
    {
        const auto in = makeNoise (N, 11);
        Signal reference;

        for (int variant = 0; variant < 3; ++variant)
        {
            GrainStretcher e;
            configure (e);
            e.setLoopLength (48000.0);
            e.setSpeed (0.25);
            e.setFrozen (false);
            e.setScatter (variant == 2, 3000.0, 5.0, 0, 0.5, 0.0);

            const auto out = run (e, in, { { S, [] (GrainStretcher& g) { g.setActive (true); } } }, 512);

            if (variant == 0)
            {
                reference = out;
            }
            else
            {
                bool same = true;
                for (size_t i = 0; i < out.size(); ++i) same = same && out[i] == reference[i];
                CHECK (same == (variant == 1), variant == 1 ? "deux passes identiques donnent deux sorties differentes" : "la dispersion ne change rien");
            }

            CHECK (allFinite (out), "sortie non finie");
            checkHealthy (e, "dispersion");
        }
    }
}

static void testHostSync()
{
    std::printf ("14. Grains cales sur la grille de l'hote\n");
    const int N = 96000, blockSize = 441;
    const double period = 3000.0;

    GrainStretcher e;
    configure (e);
    e.setLoopLength (48000.0);
    e.setSpeed (0.5);
    e.setGrainPeriod (period);
    e.setGrainSize (0.25);   // un grain bref toutes les 3000 échantillons : on voit les départs
    e.setShape (0.0);

    Signal out = makeNoise (N, 12);
    const int S = 10 * blockSize;

    for (int start = 0; start < N; start += blockSize)
    {
        const int n = std::min (blockSize, N - start);
        if (start == S) e.setActive (true);
        e.syncGrainPhase ((double) start / period + 0.25);   // la grille est décalée d'un quart de période
        float* channel[1] = { out.data() + start };
        e.process (channel, 1, n);
    }

    // Départs attendus : là où (t / 3000 + 0,25) est entier, soit t = 2250 + 3000 k. Chaque grain dure 750 échantillons.
    int onGrid = 0, offGrid = 0;

    for (int t = S + 6000; t < N - 1; ++t)
    {
        const bool silentBefore = out[(size_t) (t - 1)] == 0.0f && out[(size_t) (t - 2)] == 0.0f && out[(size_t) (t - 3)] == 0.0f;

        if (silentBefore && out[(size_t) t] != 0.0f)
        {
            const double offset = std::fmod ((double) t - 2250.0, period);
            const double distance = std::min (offset, period - offset);
            (distance <= 3.0 ? onGrid : offGrid)++;
        }
    }

    CHECK (onGrid >= 25, "%d departs sur la grille (attendu : une trentaine)", onGrid);
    CHECK (offGrid == 0, "%d departs hors grille", offGrid);
    checkHealthy (e, "grille");
}

static void testLoadCapture()
{
    std::printf ("15. Capture rechargee (projet rouvert) : la lecture reprend dessus, sans recapturer\n");
    const int pre = 4000, count = 60000, P = 1024;
    const auto saved = makeNoise (count, 13);

    GrainStretcher e;
    configure (e);
    e.setLoopLength (20000.0);
    e.setFrozen (true);
    e.setPosition (0.25);
    e.setGrainPeriod ((double) P);
    e.setGrainSize (1.0);
    e.setShape (0.0);

    const float* source[1] = { saved.data() };
    e.loadCapture (source, 1, count, pre);
    e.setActive (true);

    CHECK (e.isRunning(), "le moteur devrait tourner des le chargement");

    Signal out ((size_t) 30000, 0.0f);   // entrée silencieuse : tout ce qui sort vient de la capture
    float* channel[1] = { out.data() };
    e.process (channel, 1, 30000);

    float worst = 0.0f;
    for (int t0 = 4 * P; t0 + P < 30000; t0 += P)
        for (int k = 60; k < P - 60; ++k)
            worst = std::max (worst, std::abs (out[(size_t) (t0 + k)] - saved[(size_t) (pre + 5000 + k)]));

    CHECK (worst == 0.0f, "la capture rechargee n'est pas lue au bon endroit (ecart %g)", (double) worst);
    CHECK (e.getCaptureCount() == 1, "une recapture a eu lieu (%u)", e.getCaptureCount());
    CHECK (std::abs (out[0]) > 0.0f || std::abs (out[100]) > 0.0f, "pas de son des le premier bloc");

    GrainStretcher::View view;
    CHECK (e.getView (view) && view.running && view.origin == pre && view.end == count && view.begin == 0, "vue incoherente apres chargement");

    // Une recapture après un chargement passe sur l'autre bande, et la vue le signale.
    const auto before = e.getLayoutSeq();
    e.trigger();
    Signal more = makeNoise (5000, 14);
    float* channel2[1] = { more.data() };
    e.process (channel2, 1, 5000);
    CHECK (e.getLayoutSeq() != before && (e.getLayoutSeq() & 1u) == 0, "le changement de bande n'est pas signale");
    CHECK (allFinite (more), "sortie non finie");
    checkHealthy (e, "chargement");

    // reset() : les grains sont coupés, la capture reste.
    e.reset();
    CHECK (e.isRunning() && e.getNumActiveGrains() == 0, "reset : etat inattendu");
    e.process (channel2, 1, 5000);
    CHECK (e.getNumActiveGrains() > 0 && e.getCaptureCount() == 2, "reset : la lecture ne reprend pas sur la capture");
}

static void testFilterAndLimiter()
{
    std::printf ("16. Filtre et limiteur de sortie\n");

    // Filtre éteint, limiteur allumé, signal sous le plafond : intact.
    {
        OutputFilter f;
        f.prepare (kSampleRate);
        f.setActive (false);
        f.setLimiter (true);
        f.reset();

        const auto in = makeNoise (20000, 15, 0.9f);
        auto out = in;
        float* channel[1] = { out.data() };
        f.process (channel, 1, (int) out.size());

        bool same = true;
        for (size_t i = 0; i < in.size(); ++i) same = same && in[i] == out[i];
        CHECK (same, "sous le plafond, le limiteur modifie le signal");
    }

    // Filtre éteint, limiteur allumé, signal trop fort : ramené sous le plafond.
    {
        OutputFilter f;
        f.prepare (kSampleRate);
        f.setActive (false);
        f.setLimiter (true);
        f.reset();

        auto out = makeSine (48000, 220.0, 2.5f);
        float* channel[1] = { out.data() };
        f.process (channel, 1, (int) out.size());
        CHECK (peakOf (out, 0, 48000) <= 1.0001f, "limiteur : crete %g", (double) peakOf (out, 0, 48000));
        CHECK (peakOf (out, 24000, 48000) > 0.9f, "limiteur : niveau ecrase (%g)", (double) peakOf (out, 24000, 48000));

        f.setLimiter (false);
        f.reset();
        auto loud = makeSine (48000, 220.0, 2.5f);
        const auto reference = loud;
        float* channel2[1] = { loud.data() };
        f.process (channel2, 1, (int) loud.size());
        bool same = true;
        for (size_t i = 0; i < loud.size(); ++i) same = same && loud[i] == reference[i];
        CHECK (same, "limiteur eteint : le signal est modifie");
    }

    // Filtre allumé : chaque type atténue ce qu'il doit.
    struct Case { int type; bool steep; double low, high; const char* name; };

    for (const auto c : { Case { OutputFilter::highPass, false, -24.0, -0.5, "passe-haut 12 dB" },
                          Case { OutputFilter::highPass, true, -48.0, -0.5, "passe-haut 24 dB" },
                          Case { OutputFilter::lowPass, false, -0.5, -24.0, "passe-bas 12 dB" },
                          Case { OutputFilter::lowPass, true, -0.5, -48.0, "passe-bas 24 dB" } })
    {
        double gains[2] = {};
        int index = 0;

        for (const double frequency : { 250.0, 4000.0 })
        {
            OutputFilter f;
            f.prepare (kSampleRate);
            f.setActive (true);
            f.setType (c.type);
            f.setSteep (c.steep);
            f.setFrequency (1000.0);
            f.setResonance (0.0);
            f.setLimiter (true);
            f.reset();

            auto out = makeSine (48000, frequency, 0.5f);
            float* channel[1] = { out.data() };
            f.process (channel, 1, (int) out.size());
            gains[index++] = decibels (rmsOf (out, 24000, 48000) / (0.5 / std::sqrt (2.0)));
        }

        const double expectedLow = c.low, expectedHigh = c.high;
        CHECK (std::abs (gains[0] - expectedLow) < 3.0, "%s : %.1f dB a 250 Hz (attendu %.1f)", c.name, gains[0], expectedLow);
        CHECK (std::abs (gains[1] - expectedHigh) < 3.0, "%s : %.1f dB a 4 kHz (attendu %.1f)", c.name, gains[1], expectedHigh);
    }
}

static void testFuzz()
{
    std::printf ("17. Reglages au hasard, toutes frequences d'echantillonnage et tailles de bloc\n");
    std::mt19937 rng (2026);
    const auto pick = [&rng] (double low, double high) { return std::uniform_real_distribution<double> (low, high) (rng); };
    const auto chance = [&rng] (double probability) { return std::uniform_real_distribution<double> (0.0, 1.0) (rng) < probability; };

    int runs = 0;

    for (const double rate : { 8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int channels = 1; channels <= 2; ++channels)
        {
            GrainStretcher e;
            e.prepare (rate, channels, rate > 100000.0 ? 4.0 : 10.0);
            OutputFilter f;
            f.prepare (rate);

            const int N = (int) (rate * (rate > 50000.0 ? 2.0 : 5.0));
            Signal left = makeNoise (N, 100u + (unsigned) runs, 0.7f), right = makeNoise (N, 200u + (unsigned) runs, 0.7f);
            float peak = 0.0f;

            for (int start = 0; start < N;)
            {
                const int n = std::min (N - start, chance (0.1) ? 1 : (int) pick (1.0, 2049.0));

                if (chance (0.5))
                {
                    e.setLoopLength (std::exp (pick (std::log (20.0), std::log (rate * 12.0))));
                    e.setAutoCapture (chance (0.4), std::exp (pick (std::log (30.0), std::log (rate * 3.0))));
                    e.setSpeed (chance (0.2) ? 1.0 : std::exp (pick (std::log (0.001), std::log (4.0))));
                    e.setFrozen (chance (0.15));
                    e.setBackward (chance (0.3));
                    e.setPosition (pick (0.0, 1.0));
                    e.setGrainPeriod (std::exp (pick (std::log (4.0), std::log (rate * 5.0))));
                    e.setGrainSize (pick (0.0, 9.0));
                    e.setShape (pick (-0.1, 1.1));
                    e.setTilt (pick (-1.2, 1.2));
                    e.setPitch (pick (-50.0, 50.0));
                    e.setFlip (chance (0.3));
                    e.setScatter (chance (0.6), pick (0.0, rate * 2.0), pick (0.0, 30.0), (int) pick (0.0, 3.0), pick (0.0, 1.0), pick (0.0, 1.0));
                    e.setActive (chance (0.85));
                    f.setActive (chance (0.5));
                    f.setLimiter (false);
                    f.setFrequency (std::exp (pick (std::log (20.0), std::log (20000.0))));
                    f.setResonance (pick (0.0, 1.0));
                    f.setType ((int) pick (0.0, 3.0));
                }

                if (chance (0.1))  e.trigger();
                if (chance (0.05)) e.syncGrainPhase (pick (0.0, 1.0));
                if (chance (0.01)) e.reset();
                if (chance (0.004)) e.clear();
                if (chance (0.02)) e.setLoopLength (std::numeric_limits<double>::quiet_NaN());
                if (chance (0.02)) e.setGrainPeriod (std::numeric_limits<double>::infinity());

                float* channel[2] = { left.data() + start, right.data() + start };
                e.process (channel, channels, n);

                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < n; ++i)
                        peak = std::max (peak, std::abs (channel[ch][i]));

                f.process (channel, channels, n);
                start += n;
            }

            CHECK (allFinite (left) && (channels == 1 || allFinite (right)), "%.0f Hz, %d canal(aux) : sortie non finie", rate, channels);
            CHECK (peak < 6.0f, "%.0f Hz, %d canal(aux) : crete %g", rate, channels, (double) peak);
            CHECK (e.getOutOfRangeReads() == 0, "%.0f Hz, %d canal(aux) : %d lectures hors zone", rate, channels, e.getOutOfRangeReads());
            CHECK (e.getStolenGrains() == 0, "%.0f Hz, %d canal(aux) : %d grains coupes", rate, channels, e.getStolenGrains());
            ++runs;
        }
    }
}

static void benchmark()
{
    GrainStretcher e;
    configure (e, kSampleRate, 2);
    e.setLoopLength (48000.0);
    e.setSpeed (0.125);
    e.setGrainPeriod (480.0);
    e.setGrainSize (8.0);
    e.setPitch (7.0);
    e.setScatter (true, 2000.0, 3.0, 0, 0.5, 1.0);
    e.setActive (true);

    const int N = 20 * 48000;
    Signal left = makeNoise (N, 20), right = makeNoise (N, 21);
    const auto begin = std::chrono::steady_clock::now();
    runStereo (e, left, right, {}, 512);
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - begin).count();

    std::printf ("Charge (stereo, 48 kHz, 8 grains superposes, transposes et disperses) : %.2f %% d'un coeur\n", 100.0 * seconds / 20.0);
}

int main()
{
    testPassthrough();
    testUnityIsTransparent();
    testAddressing();
    testStretchKeepsPitch();
    testPitch();
    testPlayhead();
    testFirstPassNeverReadsAhead();
    testAutoCapture();
    testTapeSwitching();
    testLevels();
    testNoClicks();
    testStereoWidth();
    testScatter();
    testHostSync();
    testLoadCapture();
    testFilterAndLimiter();
    testFuzz();
    benchmark();

    if (failures == 0)
        std::printf ("\nTous les tests passent.\n");
    else
        std::printf ("\n%d echec(s).\n", failures);

    return failures == 0 ? 0 : 1;
}
