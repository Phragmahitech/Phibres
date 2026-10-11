#pragma once

#include <algorithm>
#include <cmath>

/**
    Filtre de fin de chaîne : passe-haut, passe-bande ou passe-bas résonant,
    en 12 ou 24 dB par octave, suivi d'un limiteur de sécurité
    (aucune dépendance JUCE, testable seul).

    Deux filtres à variable d'état en forme TPT, en série : ils restent stables
    quand la fréquence est balayée vite. Le premier porte la résonance ; le
    second (pente 24 dB) est réglé sans résonance, pour que le pic ne double pas.
    La fréquence et la résonance sont lissées pour éviter les bruits de potard ;
    l'allumage, l'extinction, le changement de type ou de pente passent par un
    fondu court.

    Limiteur : attaque instantanée, relâchement de 100 ms, plafond à 0 dBFS. Il
    ne touche pas au signal tant que celui-ci reste sous le plafond, et il
    travaille que le filtre soit allumé ou non (les grains qui se superposent
    peuvent dépasser le plafond sans aucun filtre).

    Filtre éteint et signal sous le plafond : le son passe intact, à
    l'échantillon près.
*/
class OutputFilter
{
public:
    enum Type { highPass = 0, bandPass = 1, lowPass = 2 };

    static constexpr double minFrequency = 20.0;
    static constexpr double maxFrequency = 20000.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        mixStep = (float) (1.0 / (0.005 * sr));                 // 5 ms
        smoothing = 1.0 - std::exp (-1.0 / (0.015 * sr));       // ~15 ms
        limiterRelease = std::exp (-1.0 / (0.1 * sr));          // 100 ms
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : first)
            s = {};

        for (auto& s : second)
            s = {};

        mix = 0.0f;

        for (int t = 0; t < 3; ++t)
            weight[t] = t == type ? 1.0f : 0.0f;

        steepMix = steep ? 1.0f : 0.0f;
        limitMix = limiting ? 1.0f : 0.0f;
        envelope = 0.0;
        logFrequencyNow = logFrequencyTarget;
        dampingNow = dampingTarget;
    }

    void setActive (bool shouldFilter) noexcept          { active = shouldFilter; }

    /** highPass, bandPass ou lowPass. */
    void setType (int newType) noexcept                  { type = std::max (0, std::min (2, newType)); }

    /** false = 12 dB par octave, true = 24 dB par octave. */
    void setSteep (bool shouldBeSteep) noexcept          { steep = shouldBeSteep; }

    /** Limiteur de sécurité sur la sortie du filtre. */
    void setLimiter (bool shouldLimit) noexcept          { limiting = shouldLimit; }

    void setFrequency (double hertz) noexcept
    {
        const double f = std::isnan (hertz) ? 1000.0 : hertz;
        logFrequencyTarget = std::log (std::max (minFrequency, std::min (f, maxFrequency)));
    }

    /** Résonance de 0 (aucune, Q = 0,7) à 1 (forte, Q = 10). */
    void setResonance (double amount) noexcept
    {
        dampingTarget = 1.0 / qualityFor (amount);
    }

    static double qualityFor (double amount) noexcept
    {
        const double a = std::isnan (amount) ? 0.0 : std::max (0.0, std::min (1.0, amount));
        return 0.7071 * std::pow (10.0 / 0.7071, a);
    }

    /** Gain du filtre à une fréquence donnée (pour dessiner sa courbe). */
    static double magnitudeAt (double hertz, double cutoff, double quality, int filterType, bool isSteep) noexcept
    {
        const double w = hertz / cutoff;

        const auto stage = [w, filterType] (double q, bool unityBand)
        {
            const double denominator = std::sqrt ((1.0 - w * w) * (1.0 - w * w) + (w / q) * (w / q));

            // Passe-bande : le pic vaut racine(Q), pour que la résonance s'entende sans exploser le niveau.
            if (filterType == bandPass)
                return (unityBand ? w / q : w / std::sqrt (q)) / denominator;

            return (filterType == lowPass ? 1.0 : w * w) / denominator;
        };

        const double gain = stage (quality, false);
        return isSteep ? gain * stage (flatQuality, true) : gain;
    }

    /** Traitement en place. */
    void process (float* const* io, int numChannels, int numSamples) noexcept
    {
        const int numCh = std::min (numChannels, maxChannels);
        const double maxCutoff = std::min (maxFrequency, sr * 0.45);
        const double k2 = 1.0 / flatQuality;

        for (int n = 0; n < numSamples; ++n)
        {
            mix = ramp (mix, active);
            steepMix = ramp (steepMix, steep);
            limitMix = ramp (limitMix, limiting);

            for (int t = 0; t < 3; ++t)
                weight[t] = ramp (weight[t], t == type);

            logFrequencyNow += (logFrequencyTarget - logFrequencyNow) * smoothing;
            dampingNow += (dampingTarget - dampingNow) * smoothing;

            const double g = std::tan (pi * std::min (std::exp (logFrequencyNow), maxCutoff) / sr);
            const double k = dampingNow;
            const double a1 = 1.0 / (1.0 + g * (g + k));
            const double a2 = g * a1;
            const double a3 = g * a2;
            const double b1 = 1.0 / (1.0 + g * (g + k2));
            const double b2 = g * b1;
            const double b3 = g * b2;
            const double bandGain = std::sqrt (k);

            double out[maxChannels] = {};
            double peak = 0.0;

            for (int ch = 0; ch < numCh; ++ch)
            {
                const double x = (double) io[ch][n];

                // Premier étage : porte la résonance.
                auto& s = first[ch];
                const double v3 = x - s.ic2;
                const double v1 = a1 * s.ic1 + a2 * v3;
                const double v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
                s.ic1 = 2.0 * v1 - s.ic1;
                s.ic2 = 2.0 * v2 - s.ic2;

                const double one = (double) weight[highPass] * (x - k * v1 - v2)
                                 + (double) weight[bandPass] * (v1 * bandGain)
                                 + (double) weight[lowPass] * v2;

                // Second étage, sans résonance : 24 dB par octave.
                auto& r = second[ch];
                const double w3 = one - r.ic2;
                const double w1 = b1 * r.ic1 + b2 * w3;
                const double w2 = r.ic2 + b2 * r.ic1 + b3 * w3;
                r.ic1 = 2.0 * w1 - r.ic1;
                r.ic2 = 2.0 * w2 - r.ic2;

                const double two = (double) weight[highPass] * (one - k2 * w1 - w2)
                                 + (double) weight[bandPass] * (w1 * k2)
                                 + (double) weight[lowPass] * w2;

                const double filtered = one + (double) steepMix * (two - one);

                out[ch] = mix <= 0.0f ? x
                        : mix >= 1.0f ? filtered
                                      : x + (double) mix * (filtered - x);
                peak = std::max (peak, std::abs (out[ch]));
            }

            // Limiteur : le gain suit le niveau de crête (les deux canaux ensemble).
            envelope = std::max (peak, envelope * limiterRelease);
            const double limited = envelope > 1.0 ? 1.0 / envelope : 1.0;
            const double limit = 1.0 + (double) limitMix * (limited - 1.0);

            if (mix > 0.0f || limit < 1.0)
                for (int ch = 0; ch < numCh; ++ch)
                    io[ch][n] = (float) (limit < 1.0 ? out[ch] * limit : out[ch]);
        }
    }

private:
    static constexpr int maxChannels = 2;
    static constexpr double pi = 3.14159265358979323846;
    static constexpr double flatQuality = 0.7071;   // second étage : pas de résonance

    float ramp (float value, bool up) const noexcept
    {
        return up ? std::min (1.0f, value + mixStep) : std::max (0.0f, value - mixStep);
    }

    struct State { double ic1 = 0.0, ic2 = 0.0; };
    State first[maxChannels], second[maxChannels];

    double sr = 44100.0;
    double logFrequencyTarget = 6.907755, logFrequencyNow = 6.907755;   // ln(1000)
    double dampingTarget = 1.0 / 0.7071, dampingNow = 1.0 / 0.7071;
    double smoothing = 1.0, limiterRelease = 0.0, envelope = 0.0;
    float mix = 0.0f, steepMix = 0.0f, limitMix = 0.0f, mixStep = 1.0f;
    float weight[3] = { 1.0f, 0.0f, 0.0f };
    int type = highPass;
    bool active = false, steep = false, limiting = false;
};
