#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

/**
    Moteur de Phibres : étirement du temps par grains, sur une capture du son
    qui passe (aucune dépendance JUCE, testable seul).

    Principe
    --------
    À l'activation, le moteur enregistre l'entrée sur une « bande ». Une tête de
    lecture parcourt la boucle [0, longueur) de cette capture à la vitesse
    voulue (1 = vitesse réelle, 1/8 = huit fois plus lent, 0 = gel, négative =
    marche arrière). À intervalles réguliers (la « période »), un grain part de
    l'endroit où se trouve la tête et relit la bande à la hauteur voulue,
    indépendante de la vitesse de la tête. La sortie est la somme des grains.

    La tête ne lit jamais le futur : tant que la capture est en cours, un grain
    qui dépasserait ce qui est déjà enregistré est reculé (il lit alors le bord
    le plus récent, voire un peu avant la capture) ou raccourci.

    Bandes
    ------
    Deux bandes linéaires. Une recapture déplace simplement l'origine au point
    d'écriture de la bande en cours, qui continue d'enregistrer : les grains
    déjà partis gardent leurs données intactes. On ne change de bande que
    lorsqu'il ne reste plus assez de place ; la capacité est choisie pour que
    tous les grains de l'autre bande soient alors terminés. La bande quittée
    finit de s'enregistrer jusqu'au bout, pour ses grains encore en vol.
    Chaque bande commence par une seconde d'historique (ce qui précédait la
    capture), tenu à jour en permanence, même moteur éteint.

    Éteint, le signal passe intact, à l'échantillon près, et sans latence.
*/
class GrainStretcher
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int maxGrains = 48;
    static constexpr int minGrainLength = 8;
    static constexpr double historySeconds = 1.0;
    static constexpr double maxGrainSeconds = 4.0;
    static constexpr double postRollSeconds = 2.0;
    static constexpr double maxSpeed = 4.0;
    static constexpr double minSizeRatio = 0.1;
    static constexpr double maxSizeRatio = 8.0;
    static constexpr double maxPitch = 48.0;

    enum DetuneMode { detuneFine = 0, detuneSemitones = 1, detuneOctaves = 2 };

    //==========================================================================
    void prepare (double sampleRate, int numChannels, double maxCaptureSeconds = 30.0)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        channels = std::max (1, std::min (maxChannels, numChannels));

        historyLength = std::max (16, (int) std::ceil (historySeconds * sr));
        maxLoop = std::max (64, (int) std::ceil (std::max (0.1, maxCaptureSeconds) * sr));
        maxGrain = std::max (minGrainLength, (int) (maxGrainSeconds * sr));
        postRoll = (int) (postRollSeconds * sr);
        capacity = historyLength + maxLoop + maxGrain + postRoll + 16;

        for (auto& tape : tapes)
            for (int ch = 0; ch < maxChannels; ++ch)
                tape.data[ch].assign (ch < channels ? (size_t) capacity : (size_t) 0, 0.0f);

        for (int ch = 0; ch < maxChannels; ++ch)
            history[ch].assign (ch < channels ? (size_t) historyLength : (size_t) 0, 0.0f);

        mixStep = (float) (1.0 / (0.005 * sr));                     // 5 ms pour entrer / sortir
        speedSmoothing = 1.0 - std::exp (-1.0 / (0.010 * sr));      // ~10 ms
        positionSmoothing = 1.0 - std::exp (-1.0 / (0.030 * sr));   // ~30 ms
        minEdge = std::max (2.0, 0.001 * sr);                       // bords d'un grain : 1 ms au moins

        for (int i = 0; i <= tableSize; ++i)
            raisedCosine[i] = (float) (0.5 - 0.5 * std::cos (pi * (double) i / tableSize));

        loopLength = std::max (64.0, std::min (loopLength, (double) maxLoop));
        clear();
    }

    /** Remise à zéro complète : plus de capture, plus d'historique. */
    void clear() noexcept
    {
        layoutSeq.fetch_add (layoutSeq.load (std::memory_order_relaxed) & 1u ? 2u : 1u, std::memory_order_release);   // impair : bandes en chantier

        for (auto& tape : tapes)
        {
            tape.begin = 0;
            tape.end = 0;
            tape.recording = false;
        }

        numActive = 0;
        cur = 0;
        origin = 0;
        historyWrite = 0;
        historyCount = 0;
        running = false;
        triggerPending = false;
        needPrefill = false;
        hostLocked = false;
        wetMix = 0.0f;
        travel = 0.0;
        phase = 0.0;
        autoCounter = 0.0;
        speedNow = signedSpeed();
        positionNow = positionTarget;
        random = 0x9e3779b9u;
        captureCount = 0;
        stolenGrains = 0;
        outOfRangeReads = 0;
        publish();
    }

    /** Coupe les grains en cours mais garde la capture (demande de l'hôte : « vide tes queues »). */
    void reset() noexcept
    {
        numActive = 0;
        needPrefill = running;
        wetMix = running && wantActive ? 1.0f : 0.0f;
    }

    //==========================================================================
    void setActive (bool shouldStretch) noexcept             { wantActive = shouldStretch; }

    /** Recapture au prochain échantillon traité (sans effet si le moteur est éteint). */
    void trigger() noexcept                                  { triggerPending = true; }

    /** Longueur de la boucle parcourue par la tête, en échantillons. */
    void setLoopLength (double lengthInSamples) noexcept
    {
        loopLength = std::max (64.0, std::min (number (lengthInSamples, 44100.0), (double) maxLoop));
    }

    double getLoopLength() const noexcept                    { return loopLength; }

    /** Recapture périodique, comptée depuis la dernière capture. */
    void setAutoCapture (bool shouldRecapture, double intervalInSamples) noexcept
    {
        autoOn = shouldRecapture;
        autoInterval = std::max (64.0, std::min (number (intervalInSamples, 44100.0), 1.0e9));
    }

    /** Vitesse de la tête : 1 = temps réel, 0,125 = étiré 8 fois. De 0 à maxSpeed. */
    void setSpeed (double newSpeed) noexcept                 { speed = std::max (0.0, std::min (number (newSpeed, 1.0), maxSpeed)); }
    void setFrozen (bool shouldFreeze) noexcept              { frozen = shouldFreeze; }
    void setBackward (bool shouldGoBackward) noexcept        { backward = shouldGoBackward; }

    /** Décalage de la tête dans la boucle, de 0 (début) à 1 (fin). */
    void setPosition (double position) noexcept              { positionTarget = unit (position, 0.0); }

    /** Intervalle entre deux départs de grain, en échantillons. */
    void setGrainPeriod (double periodInSamples) noexcept
    {
        period = std::max ((double) minGrainLength, std::min (number (periodInSamples, 2048.0), maxGrainSeconds * sr));
    }

    /**
        Cale le départ des grains sur une grille : phase (0 à 1) dans la période au
        début du prochain bloc. À appeler à chaque bloc quand l'hôte défile.
    */
    void syncGrainPhase (double hostPhase) noexcept
    {
        if (std::isnan (hostPhase))
            return;

        hostLocked = true;

        double difference = hostPhase - std::floor (hostPhase) - phase;
        difference -= std::round (difference);                // écart circulaire, de -0,5 à 0,5

        if (std::abs (difference) * period > 2.0)             // on tolère deux échantillons d'arrondi
            phase += difference;                              // peut dépasser 1 : le grain manqué part aussitôt
    }

    /** Durée d'un grain, en nombre de périodes : 2 = recouvrement de moitié. */
    void setGrainSize (double ratio) noexcept                { sizeRatio = std::max (minSizeRatio, std::min (number (ratio, 2.0), maxSizeRatio)); }

    /** Forme de la fenêtre : 0 = presque rectangulaire (bords de 1 ms), 1 = cloche (Hann). */
    void setShape (double amount) noexcept                   { shape = unit (amount, 1.0); }

    /** Bascule de la fenêtre : -1 = attaque franche et longue retombée, +1 = l'inverse. */
    void setTilt (double amount) noexcept                    { tilt = std::max (-1.0, std::min (number (amount, 0.0), 1.0)); }

    /** Hauteur des grains, en demi-tons (indépendante de la vitesse de la tête). */
    void setPitch (double semitones) noexcept                { pitch = std::max (-maxPitch, std::min (number (semitones, 0.0), maxPitch)); }

    /** Grains lus à l'envers. */
    void setFlip (bool shouldFlip) noexcept                  { flip = shouldFlip; }

    /**
        Dispersion, tirée au sort pour chaque grain :
        spray = écart de position (échantillons), detune = écart de hauteur (demi-tons),
        flipChance = probabilité d'inverser le sens du grain, width = écart de panoramique (0 à 1).
    */
    void setScatter (bool enabled, double sprayInSamples, double detuneInSemitones, int detuneQuantize,
                     double flipProbability, double stereoWidth) noexcept
    {
        scatter = enabled;
        spray = std::max (0.0, std::min (number (sprayInSamples, 0.0), 1.0e8));
        detune = std::max (0.0, std::min (number (detuneInSemitones, 0.0), maxPitch));
        detuneMode = std::max (0, std::min (2, detuneQuantize));
        flipChance = unit (flipProbability, 0.0);
        width = unit (stereoWidth, 0.0);
    }

    //==========================================================================
    /** Traitement en place. */
    void process (float* const* io, int numChannels, int numSamples) noexcept
    {
        const int numCh = std::min (numChannels, channels);

        if (numCh <= 0 || capacity <= 0)
            return;

        for (int n = 0; n < numSamples; ++n)
        {
            float in[maxChannels] = {};

            for (int ch = 0; ch < numCh; ++ch)
                in[ch] = io[ch][n];

            // --- Captures ---
            if (wantActive && ! running)
            {
                start();
            }
            else if (running && wantActive)
            {
                if (triggerPending)
                {
                    capture (false);
                    autoCounter = 0.0;
                }
                else if (autoOn)
                {
                    autoCounter += 1.0;

                    if (autoCounter >= autoInterval)
                    {
                        autoCounter -= autoInterval;

                        if (autoCounter >= autoInterval)   // l'intervalle vient de raccourcir d'un coup
                            autoCounter = 0.0;

                        capture (false);
                    }
                }
            }

            triggerPending = false;

            // --- Enregistrement : les bandes (voir capture()), et l'historique (toujours) ---
            for (auto& tape : tapes)
            {
                if (tape.recording)
                {
                    for (int ch = 0; ch < numCh; ++ch)
                        tape.data[ch][(size_t) tape.end] = in[ch];

                    if (++tape.end >= capacity)
                        tape.recording = false;
                }
            }

            for (int ch = 0; ch < numCh; ++ch)
                history[ch][(size_t) historyWrite] = in[ch];

            if (++historyWrite >= historyLength)
                historyWrite = 0;

            if (historyCount < historyLength)
                ++historyCount;

            if (! running)
                continue;   // éteint : le signal passe intact

            // --- Réglages lissés ---
            speedNow = approach (speedNow, signedSpeed(), speedSmoothing, 1.0e-7);
            positionNow = approach (positionNow, positionTarget, positionSmoothing, 1.0e-7);

            // --- Départ des grains ---
            if (needPrefill)
            {
                prefill();
                needPrefill = false;
            }
            else
            {
                phase += 1.0 / period;

                if (phase >= 1.0)
                {
                    phase -= std::floor (phase);
                    spawn (0);
                }
            }

            // --- Somme des grains ---
            float wet[maxChannels] = {};
            float windowSum = 0.0f;

            for (int i = 0; i < numActive;)
            {
                auto& grain = grains[i];
                const auto& tape = tapes[grain.tape];
                const float gainNow = windowAt (grain);
                windowSum += gainNow;
                const int lo = tape.begin, hi = tape.end - 1;
                const double floorPosition = std::floor (grain.position);
                const int index = (int) floorPosition;
                const float fraction = (float) (grain.position - floorPosition);

                if (index < lo || index > hi)
                    ++outOfRangeReads;   // ne doit jamais arriver : le départ du grain est calculé pour l'éviter

                if (fraction <= 0.0f)   // position entière : l'échantillon tel quel
                {
                    const size_t at = (size_t) clampIndex (index, lo, hi);

                    for (int ch = 0; ch < numCh; ++ch)
                        wet[ch] += tape.data[ch][at] * gainNow * grain.gain[ch];
                }
                else
                {
                    const size_t a = (size_t) clampIndex (index - 1, lo, hi), b = (size_t) clampIndex (index, lo, hi);
                    const size_t c = (size_t) clampIndex (index + 1, lo, hi), d = (size_t) clampIndex (index + 2, lo, hi);

                    for (int ch = 0; ch < numCh; ++ch)
                    {
                        const float* data = tape.data[ch].data();
                        wet[ch] += hermite (data[a], data[b], data[c], data[d], fraction) * gainNow * grain.gain[ch];
                    }
                }

                grain.position += grain.increment;

                if (++grain.index >= grain.length)
                    grains[i] = grains[--numActive];   // terminé : le dernier prend sa place
                else
                    ++i;
            }

            // --- Tête de lecture ---
            travel += speedNow;

            if (travel >= loopLength || travel < 0.0)
                travel -= loopLength * std::floor (travel / loopLength);

            // --- Sortie : fondu de 5 ms entre le son direct et les grains ---
            wetMix = wantActive ? std::min (1.0f, wetMix + mixStep) : std::max (0.0f, wetMix - mixStep);

            // Mise à niveau : dès que les fenêtres superposées dépassent 1, on divise par leur somme. Des grains qui
            // lisent le même son ressortent ainsi au niveau de l'entrée, quels que soient leur nombre et leur forme.
            const float norm = windowSum > 1.0f ? 1.0f / windowSum : 1.0f;

            for (int ch = 0; ch < numCh; ++ch)
                io[ch][n] = wetMix >= 1.0f ? wet[ch] * norm
                                           : in[ch] + wetMix * (wet[ch] * norm - in[ch]);

            if (! wantActive && wetMix <= 0.0f)
                stop();
        }

        hostLocked = false;
        publish();
    }

    //==========================================================================
    // Accès depuis un autre fil (interface, sauvegarde) : lecture seule.

    struct View
    {
        bool running = false;
        int tape = 0;             // bande en cours
        int begin = 0;            // premier échantillon valide de cette bande
        int origin = 0;           // position 0 de la capture
        int end = 0;              // un après le dernier échantillon enregistré
        double loopLength = 0.0;  // en échantillons
        double playhead = 0.0;    // tête de lecture, de 0 à 1 dans la boucle
        unsigned int captures = 0;
    };

    /** Change (et devient impair) dès qu'une bande est réécrite depuis son début : ce qu'on y lisait n'est plus valable. */
    unsigned int getLayoutSeq() const noexcept               { return layoutSeq.load (std::memory_order_acquire); }

    /** false si une bande est en cours de remplacement (réessayer plus tard). */
    bool getView (View& view) const noexcept
    {
        const auto before = getLayoutSeq();

        if (before & 1u)
            return false;

        view.running = viewRunning.load (std::memory_order_relaxed);
        view.tape = viewTape.load (std::memory_order_relaxed);
        view.begin = viewBegin.load (std::memory_order_relaxed);
        view.origin = viewOrigin.load (std::memory_order_relaxed);
        view.end = viewEnd.load (std::memory_order_relaxed);
        view.loopLength = viewLoop.load (std::memory_order_relaxed);
        view.playhead = viewHead.load (std::memory_order_relaxed);
        view.captures = viewCaptures.load (std::memory_order_relaxed);

        return getLayoutSeq() == before;
    }

    const float* getTapeData (int tape, int channel) const noexcept
    {
        return tapes[std::max (0, std::min (1, tape))].data[std::max (0, std::min (channels - 1, channel))].data();
    }

    int getNumChannels() const noexcept                      { return channels; }
    int getHistoryLength() const noexcept                    { return historyLength; }
    int getTapeCapacity() const noexcept                     { return capacity; }
    int getMaxLoopLength() const noexcept                    { return maxLoop; }

    /**
        Installe une capture toute faite (projet rouvert) et reprend la lecture dessus,
        sans recapturer. À appeler depuis le fil audio, entre deux process().
        preRoll = nombre d'échantillons qui précèdent la position 0.
    */
    void loadCapture (const float* const* source, int numSourceChannels, int numSamples, int preRoll) noexcept
    {
        if (capacity <= 0 || source == nullptr || numSourceChannels <= 0 || numSamples <= 0)
            return;

        beginLayoutChange();

        const int count = std::min (numSamples, capacity);
        auto& tape = tapes[0];

        for (int ch = 0; ch < channels; ++ch)
        {
            const float* from = source[std::min (ch, numSourceChannels - 1)];
            std::copy (from, from + count, tape.data[ch].begin());
        }

        tape.begin = 0;
        tape.end = count;
        tape.recording = false;
        tapes[1].begin = tapes[1].end = 0;
        tapes[1].recording = false;

        cur = 0;
        origin = std::max (0, std::min (preRoll, count - 1));
        numActive = 0;
        travel = 0.0;
        phase = 0.0;
        autoCounter = 0.0;
        speedNow = signedSpeed();
        positionNow = positionTarget;
        running = true;
        wantActive = true;
        wetMix = 1.0f;
        needPrefill = true;
        ++captureCount;
        publish();
    }

    //==========================================================================
    // Pour les tests
    bool isRunning() const noexcept                          { return running; }
    int getNumActiveGrains() const noexcept                  { return numActive; }
    int getStolenGrains() const noexcept                     { return stolenGrains; }
    int getOutOfRangeReads() const noexcept                  { return outOfRangeReads; }
    unsigned int getCaptureCount() const noexcept            { return captureCount; }
    double getTravel() const noexcept                        { return travel; }
    int getOrigin() const noexcept                           { return origin; }
    int getCurrentTape() const noexcept                      { return cur; }
    int getTapeEnd (int tape) const noexcept                 { return tapes[tape & 1].end; }
    int getTapeBegin (int tape) const noexcept               { return tapes[tape & 1].begin; }

    /** Durées de l'attaque et de la retombée d'une fenêtre, en fraction du grain (avant le minimum de 1 ms). */
    static void windowFractions (double shapeAmount, double tiltAmount, double& attack, double& release) noexcept
    {
        attack = 0.5 * shapeAmount * (1.0 + tiltAmount);
        release = 0.5 * shapeAmount * (1.0 - tiltAmount);
    }

private:
    //==========================================================================
    struct Tape
    {
        std::vector<float> data[maxChannels];
        int begin = 0;            // premier échantillon valide
        int end = 0;              // point d'écriture
        bool recording = false;
    };

    struct Grain
    {
        double position = 0.0, increment = 1.0;
        double attack = 1.0, release = 1.0;   // en échantillons
        int index = 0, length = 0;
        int tape = 0;
        float gain[maxChannels] = { 1.0f, 1.0f };
    };

    static constexpr double pi = 3.14159265358979323846;
    static constexpr int tableSize = 2048;

    static double number (double value, double fallback) noexcept    { return std::isfinite (value) ? value : fallback; }
    static double unit (double value, double fallback) noexcept      { return std::isfinite (value) ? std::max (0.0, std::min (1.0, value)) : fallback; }
    static int clampIndex (int index, int lo, int hi) noexcept       { return index < lo ? lo : (index > hi ? hi : index); }

    static double approach (double current, double target, double coefficient, double snap) noexcept
    {
        const double delta = target - current;
        return std::abs (delta) < snap ? target : current + delta * coefficient;
    }

    static float hermite (float ym1, float y0, float y1, float y2, float t) noexcept
    {
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

    double signedSpeed() const noexcept                      { return frozen ? 0.0 : (backward ? -speed : speed); }

    float cosineAt (double x) const noexcept
    {
        const double scaled = std::max (0.0, std::min (1.0, x)) * tableSize;
        const int i = std::min (tableSize - 1, (int) scaled);
        const float f = (float) (scaled - i);
        return raisedCosine[i] + f * (raisedCosine[i + 1] - raisedCosine[i]);
    }

    float windowAt (const Grain& grain) const noexcept
    {
        const double i = (double) grain.index;

        if (i < grain.attack)
            return cosineAt (i / grain.attack);

        const double remaining = (double) (grain.length - grain.index);
        return remaining <= grain.release ? cosineAt (remaining / grain.release) : 1.0f;
    }

    // Tirages : un générateur simple, toujours le même au départ (tests reproductibles).
    double uniform() noexcept
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return (double) (random >> 8) * (1.0 / 16777216.0);   // 0 à 1 exclu
    }

    double bipolar() noexcept                                { return 2.0 * uniform() - 1.0; }

    //==========================================================================
    void beginLayoutChange() noexcept
    {
        if ((layoutSeq.load (std::memory_order_relaxed) & 1u) == 0)
            layoutSeq.fetch_add (1u, std::memory_order_release);
    }

    void publish() noexcept
    {
        const auto& tape = tapes[cur];

        viewRunning.store (running, std::memory_order_relaxed);
        viewTape.store (cur, std::memory_order_relaxed);
        viewBegin.store (tape.begin, std::memory_order_relaxed);
        viewOrigin.store (origin, std::memory_order_relaxed);
        viewEnd.store (tape.end, std::memory_order_relaxed);
        viewLoop.store (loopLength, std::memory_order_relaxed);
        viewHead.store (headInLoop() / loopLength, std::memory_order_relaxed);
        viewCaptures.store (captureCount, std::memory_order_relaxed);

        if (layoutSeq.load (std::memory_order_relaxed) & 1u)
            layoutSeq.fetch_add (1u, std::memory_order_release);   // de nouveau pair : la vue est cohérente
    }

    /** Position de la tête dans la boucle, en échantillons (décalage compris). */
    double headInLoop() const noexcept
    {
        const double p = positionNow * loopLength + travel;
        return p - loopLength * std::floor (p / loopLength);
    }

    /** Redémarre une bande : l'historique d'abord, la capture ensuite. */
    void startTape (int index) noexcept
    {
        beginLayoutChange();

        auto& tape = tapes[index];
        const int count = std::min (historyCount, historyLength);

        tape.begin = historyLength - count;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* destination = tape.data[ch].data();

           #ifdef PHIBRES_POISON_TAPES
            std::fill (tape.data[ch].begin(), tape.data[ch].end(), std::numeric_limits<float>::quiet_NaN());
           #endif

            int from = historyWrite - count;

            if (from < 0)
                from += historyLength;

            for (int i = 0; i < count; ++i)
            {
                destination[tape.begin + i] = history[ch][(size_t) from];

                if (++from >= historyLength)
                    from = 0;
            }
        }

        tape.end = historyLength;
        tape.recording = true;
        origin = historyLength;
    }

    /** Nouvelle capture à partir de maintenant. */
    void capture (bool fresh) noexcept
    {
        auto& current = tapes[cur];
        const int needed = std::min (maxLoop, (int) loopLength) + postRoll;

        if (! fresh && current.recording && capacity - current.end >= needed)
        {
            origin = current.end;   // même bande : elle continue d'enregistrer, les grains en vol gardent leurs données
        }
        else
        {
            const int next = 1 - cur;

            // Par construction il n'y a plus de grain sur l'autre bande ; s'il en restait, on les coupe.
            for (int i = numActive; --i >= 0;)
            {
                if (grains[i].tape == next)
                {
                    grains[i] = grains[--numActive];
                    ++stolenGrains;
                }
            }

            // La bande qu'on quitte continue d'enregistrer jusqu'au bout : ses grains en vol, partis au bord
            // de l'enregistrement, comptent sur la suite.
            startTape (next);
            cur = next;
        }

        travel = 0.0;
        ++captureCount;
    }

    void start() noexcept
    {
        numActive = 0;
        running = true;
        capture (true);
        autoCounter = 0.0;
        speedNow = signedSpeed();
        positionNow = positionTarget;
        wetMix = 0.0f;
        needPrefill = true;

        if (! hostLocked)
            phase = 0.0;
    }

    void stop() noexcept
    {
        running = false;
        numActive = 0;
        tapes[0].recording = false;
        tapes[1].recording = false;
    }

    /** Remplit d'un coup le nuage de grains, comme s'il tournait déjà : pas de trou à l'allumage. */
    void prefill() noexcept
    {
        const int length = nominalLength();

        for (int j = 0; j < maxGrains; ++j)
        {
            const int elapsed = (int) std::llround ((phase + j) * period);

            if (elapsed >= length)
                break;

            spawn (elapsed);
        }
    }

    int nominalLength() const noexcept
    {
        return std::max (minGrainLength, std::min ((int) std::llround (period * sizeRatio), maxGrain));
    }

    /** Attaque et retombée d'un grain de cette longueur, en échantillons. */
    void edges (int length, double& attack, double& release) const noexcept
    {
        double attackFraction = 0.0, releaseFraction = 0.0;
        windowFractions (shape, tilt, attackFraction, releaseFraction);

        const double shortest = std::min (minEdge, 0.5 * length);
        attack = std::max (shortest, attackFraction * length);
        release = std::max (shortest, releaseFraction * length);

        if (attack + release > (double) length)
        {
            const double scale = (double) length / (attack + release);
            attack *= scale;
            release *= scale;
        }
    }

    /** Lance un grain. elapsed > 0 : il démarre comme s'il était parti depuis ce nombre d'échantillons. */
    void spawn (int elapsed) noexcept
    {
        if (numActive >= maxGrains)
            return;

        const auto& tape = tapes[cur];
        const int lo = tape.begin, hi = tape.end - 1;

        if (hi < lo)
            return;

        int length = nominalLength();

        if (elapsed >= length)
            return;

        // --- Tirages ---
        double semitones = pitch;
        bool flipped = flip;
        double offset = 0.0, pan = 0.0;

        if (scatter)
        {
            const double a = bipolar(), b = bipolar(), c = uniform(), d = bipolar();

            offset = spray * a;

            double shift = detune * b;

            if (detuneMode == detuneSemitones)
                shift = std::round (shift);
            else if (detuneMode == detuneOctaves)
                shift = 12.0 * std::round (shift / 12.0);

            semitones += shift;

            if (c < flipChance)
                flipped = ! flipped;

            pan = width * d;
        }

        const double rate = std::exp2 (std::max (-maxPitch, std::min (semitones, maxPitch)) / 12.0);

        // --- Position de départ, ramenée dans ce qui est déjà enregistré ---
        int remaining = length - elapsed;
        const double steps = (double) (remaining - 1);
        const double growth = tape.recording ? 1.0 : 0.0;        // la bande avance d'un échantillon par échantillon lu
        double startPosition = (double) origin + headInLoop() + offset;
        double increment = rate;

        if (! flipped)
        {
            // Le grain lit start + k * rate : il ne doit jamais doubler le point d'écriture, ni sortir de la bande.
            // Dès qu'il avance plus vite que l'enregistrement, on garde un échantillon de marge pour les arrondis.
            const double lead = std::max (0.0, rate - growth);
            const double margin = lead > 0.0 ? 1.0 : 0.0;
            double latest = (double) hi - margin - steps * lead;

            if (tape.recording)
                latest = std::min (latest, (double) (capacity - 2) - steps * rate);

            startPosition = std::min (startPosition, latest);

            if (startPosition < (double) lo)
            {
                startPosition = (double) lo;

                double room = lead > 0.0 ? ((double) hi - margin - startPosition) / lead : 1.0e12;

                if (tape.recording)
                    room = std::min (room, ((double) (capacity - 2) - startPosition) / rate);

                remaining = (int) std::min ((double) remaining, std::floor (std::max (0.0, room)) + 1.0);
            }
        }
        else
        {
            // Grain à l'envers : il couvre le même passage, lu de la fin vers le début.
            double top = std::min (startPosition + steps * rate, (double) hi);
            const double floorLimit = (double) lo + 1.0;

            if (top - steps * rate < floorLimit)
            {
                top = std::max (top, (double) lo);
                remaining = (int) std::min ((double) remaining, std::floor (std::max (0.0, top - floorLimit) / rate) + 1.0);
            }

            startPosition = top;
            increment = -rate;
        }

        length = elapsed + remaining;

        if (remaining < minGrainLength)
            return;

        auto& grain = grains[numActive++];
        grain.position = startPosition;
        grain.increment = increment;
        grain.index = elapsed;
        grain.length = length;
        grain.tape = cur;
        edges (length, grain.attack, grain.release);

        // Panoramique : le canal opposé s'efface, l'autre reste à 1. Aucun canal ne dépasse jamais le niveau du centre.
        grain.gain[0] = grain.gain[1] = 1.0f;

        if (channels >= 2 && (pan > 0.0 || pan < 0.0))
            grain.gain[pan > 0.0 ? 0 : 1] = (float) std::cos (0.5 * pi * std::min (1.0, std::abs (pan)));
    }

    //==========================================================================
    Tape tapes[2];
    std::vector<float> history[maxChannels];
    Grain grains[maxGrains];
    float raisedCosine[tableSize + 1] = {};

    double sr = 44100.0;
    int channels = 0, capacity = 0, historyLength = 0, maxLoop = 64, maxGrain = minGrainLength, postRoll = 0;
    int historyWrite = 0, historyCount = 0;
    int cur = 0, origin = 0, numActive = 0, stolenGrains = 0, outOfRangeReads = 0;
    unsigned int captureCount = 0;
    uint32_t random = 0x9e3779b9u;

    // Réglages
    bool wantActive = false, autoOn = false, frozen = false, backward = false, flip = false, scatter = false;
    double loopLength = 44100.0, autoInterval = 44100.0;
    double speed = 1.0, positionTarget = 0.0, period = 2048.0, sizeRatio = 2.0, shape = 1.0, tilt = 0.0, pitch = 0.0;
    double spray = 0.0, detune = 0.0, flipChance = 0.0, width = 0.0;
    int detuneMode = detuneFine;

    // État
    bool running = false, triggerPending = false, needPrefill = false, hostLocked = false;
    double travel = 0.0, phase = 0.0, autoCounter = 0.0;
    double speedNow = 1.0, positionNow = 0.0;
    double speedSmoothing = 1.0, positionSmoothing = 1.0, minEdge = 44.0;
    float wetMix = 0.0f, mixStep = 1.0f;

    // Vue publiée à la fin de chaque bloc
    std::atomic<unsigned int> layoutSeq { 0 };
    std::atomic<bool> viewRunning { false };
    std::atomic<int> viewTape { 0 }, viewBegin { 0 }, viewOrigin { 0 }, viewEnd { 0 };
    std::atomic<double> viewLoop { 44100.0 }, viewHead { 0.0 };
    std::atomic<unsigned int> viewCaptures { 0 };
};
