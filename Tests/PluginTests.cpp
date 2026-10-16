// Tests du plugin complet (processeur JUCE + interface), sans DAW.
// Usage : PhibresPluginTests [dossier où écrire les captures de l'interface]

#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>

static int failures = 0;

static void check (bool condition, const juce::String& message)
{
    if (! condition)
    {
        ++failures;
        std::cout << "  ECHEC : " << message << std::endl;
    }
}

using Ids = PhibresAudioProcessor;

struct FixedTempoPlayHead final : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<double> beats;   // position en noires, si l'hôte la fournit

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (true);
        info.setPpqPosition (beats);
        return info;
    }
};

static PhibresAudioProcessor& phibres (juce::AudioProcessor& p)    { return dynamic_cast<PhibresAudioProcessor&> (p); }

static void setParam (juce::AudioProcessor& p, const char* id, float realValue)
{
    auto* param = phibres (p).apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (realValue));
}

static juce::String textOf (juce::AudioProcessor& p, const char* id)
{
    return phibres (p).apvts.getParameter (id)->getCurrentValueAsText();
}

// Fait passer "in" dans le plugin, bloc par bloc ; "onBlock" est appelé avant chaque bloc et peut y glisser du MIDI.
static void process (juce::AudioProcessor& p, const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out, int blockSize,
                     const std::function<void (int, juce::MidiBuffer&)>& onBlock = {})
{
    out.makeCopyOf (in);

    for (int start = 0; start < in.getNumSamples(); start += blockSize)
    {
        const int n = juce::jmin (blockSize, in.getNumSamples() - start);
        juce::MidiBuffer midi;

        if (onBlock)
            onBlock (start, midi);

        juce::AudioBuffer<float> block (out.getArrayOfWritePointers(), out.getNumChannels(), start, n);
        p.processBlock (block, midi);
    }
}

static juce::AudioBuffer<float> makeNoise (int numChannels, int numSamples, int seed, float amplitude = 0.5f)
{
    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    juce::Random rng (seed);

    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample (ch, i, amplitude * (rng.nextFloat() * 2.0f - 1.0f));

    return buffer;
}

static std::unique_ptr<juce::AudioProcessor> makePlugin (double sampleRate, int blockSize, int numChannels = 2)
{
    std::unique_ptr<juce::AudioProcessor> p (createPluginFilter());

    if (numChannels == 1)
    {
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add (juce::AudioChannelSet::mono());
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        check (p->setBusesLayout (mono), "le plugin refuse le mono");
    }

    p->setRateAndBufferSizeDetails (sampleRate, blockSize);
    p->prepareToPlay (sampleRate, blockSize);
    return p;
}

static bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from, int to)
{
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = from; i < to; ++i)
            if (! juce::exactlyEqual (a.getSample (ch, i), b.getSample (ch, i)))
                return false;

    return true;
}

static bool allFinite (const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite (buffer.getSample (ch, i)))
                return false;

    return true;
}

/** Un nuage sans surprise : fenêtre de Hann, recouvrement de moitié, période de 20 ms. */
static void smoothGrains (juce::AudioProcessor& p)
{
    setParam (p, Ids::grainModeId, 0.0f);
    setParam (p, Ids::grainTimeId, 20.0f);
    setParam (p, Ids::grainSizeId, 200.0f);
    setParam (p, Ids::grainShapeId, 100.0f);
    setParam (p, Ids::grainTiltId, 0.0f);
}

//==============================================================================
static void testPassthroughAndLoop (double sampleRate, int blockSize, int numChannels)
{
    std::cout << "- eteint puis boucle a vitesse 1 : " << sampleRate << " Hz, blocs de " << blockSize << ", " << numChannels << " canal(aux)" << std::endl;

    auto p = makePlugin (sampleRate, blockSize, numChannels);
    FixedTempoPlayHead playHead;
    playHead.bpm = 200.0;
    p->setPlayHead (&playHead);

    smoothGrains (*p);
    setParam (*p, Ids::lengthModeId, 1.0f);
    setParam (*p, Ids::lengthDivisionId, 5.0f);       // 1/4 : 0,3 s à 200 BPM
    setParam (*p, Ids::stretchModeId, 0.0f);
    setParam (*p, Ids::stretchFactorId, 1.0f);        // vitesse réelle

    const int L = juce::roundToInt (0.3 * sampleRate);
    const int grain = juce::roundToInt (0.04 * sampleRate);
    const int S = 6 * blockSize + 2 * juce::roundToInt (sampleRate), N = S + 4 * L;
    const auto in = makeNoise (numChannels, N, 1);
    juce::AudioBuffer<float> out;

    process (*p, in, out, blockSize, [&] (int start, juce::MidiBuffer&)
    {
        if (start >= S) setParam (*p, Ids::stretchId, 1.0f);
    });

    const int startedAt = ((S + blockSize - 1) / blockSize) * blockSize;   // l'allumage tombe en début de bloc
    check (identical (in, out, 0, startedAt), "avant l'allumage, la sortie n'est pas l'entree");

    float firstPass = 0.0f, loop = 0.0f;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        for (int t = startedAt; t < startedAt + L; ++t)
            firstPass = juce::jmax (firstPass, std::abs (out.getSample (ch, t) - in.getSample (ch, t)));

        for (int t = startedAt + L; t < N; ++t)
        {
            const int k = (t - startedAt) % L;

            if (k >= grain + 16)
                loop = juce::jmax (loop, std::abs (out.getSample (ch, t) - in.getSample (ch, startedAt + k)));
        }
    }

    check (firstPass < 6.0e-3f, "premier passage : ecart " + juce::String (firstPass));
    check (loop < 6.0e-3f, "boucle : ecart " + juce::String (loop));
    check (std::abs (phibres (*p).getLoopSeconds() - 0.3f) < 1.0e-4f, "duree de boucle : " + juce::String (phibres (*p).getLoopSeconds()));
    check (std::abs (phibres (*p).getPassSeconds() - 0.3f) < 1.0e-4f, "duree d'un passage : " + juce::String (phibres (*p).getPassSeconds()));
    check (phibres (*p).getEngine().getOutOfRangeReads() == 0, "lectures hors de la zone enregistree");
    p->setPlayHead (nullptr);
}

static void testTempoAndModes()
{
    std::cout << "- longueur, etirement et cadence des grains : free, sync, triolet, pointe, note" << std::endl;

    const double sampleRate = 48000.0;
    auto p = makePlugin (sampleRate, 512);
    FixedTempoPlayHead playHead;
    playHead.bpm = 150.0;   // une noire = 0,4 s
    p->setPlayHead (&playHead);

    auto& plugin = phibres (*p);
    const auto in = makeNoise (2, 4096, 2);
    juce::AudioBuffer<float> out;
    const auto run = [&] { process (*p, in, out, 512); };
    const auto near = [] (float a, double b) { return std::abs ((double) a - b) < 1.0e-3 * juce::jmax (1.0, b); };

    setParam (*p, Ids::lengthModeId, 1.0f);
    setParam (*p, Ids::lengthDivisionId, 3.0f);   // 1/1
    run();
    check (near (plugin.getLoopSeconds(), 1.6), "1/1 a 150 BPM : " + juce::String (plugin.getLoopSeconds()));
    check (near (plugin.getPassSeconds(), 12.8), "x8 : " + juce::String (plugin.getPassSeconds()));

    setParam (*p, Ids::lengthFeelId, 1.0f);       // triolet
    run();
    check (near (plugin.getLoopSeconds(), 1.6 * 2.0 / 3.0), "1/1 triolet : " + juce::String (plugin.getLoopSeconds()));
    setParam (*p, Ids::lengthFeelId, 2.0f);       // pointée
    run();
    check (near (plugin.getLoopSeconds(), 2.4), "1/1 pointee : " + juce::String (plugin.getLoopSeconds()));
    setParam (*p, Ids::lengthFeelId, 0.0f);

    setParam (*p, Ids::lengthDivisionId, 0.0f);   // 8/1 = 12,8 s
    run();
    check (near (plugin.getLoopSeconds(), 12.8), "8/1 : " + juce::String (plugin.getLoopSeconds()));

    playHead.bpm = 40.0;                          // 8/1 = 48 s : plus que la bande, on s'arrête à 30 s
    run();
    check (near (plugin.getLoopSeconds(), 30.0), "longueur bornee a 30 s : " + juce::String (plugin.getLoopSeconds()));
    playHead.bpm = 150.0;

    setParam (*p, Ids::lengthModeId, 0.0f);
    setParam (*p, Ids::lengthTimeId, 250.0f);
    run();
    check (near (plugin.getLoopSeconds(), 0.25), "free 250 ms : " + juce::String (plugin.getLoopSeconds()));

    // Étirement en SYNC : la boucle est étalée sur la durée choisie, quelle que soit sa longueur.
    setParam (*p, Ids::stretchModeId, 1.0f);
    setParam (*p, Ids::stretchDivisionId, 4.0f);  // 4/1 = 6,4 s
    run();
    check (near (plugin.getPassSeconds(), 6.4), "etale sur 4/1 : " + juce::String (plugin.getPassSeconds()));
    setParam (*p, Ids::stretchFeelId, 2.0f);
    run();
    check (near (plugin.getPassSeconds(), 9.6), "etale sur 4/1 pointe : " + juce::String (plugin.getPassSeconds()));
    setParam (*p, Ids::stretchFeelId, 0.0f);

    // Durée plus courte que la boucle : la tête va plus vite que le temps réel, jusqu'à quatre fois.
    setParam (*p, Ids::lengthModeId, 1.0f);
    setParam (*p, Ids::lengthDivisionId, 1.0f);   // 4/1 = 6,4 s
    setParam (*p, Ids::stretchDivisionId, 0.0f);  // sur 1/4 = 0,4 s : il faudrait 16 fois la vitesse
    run();
    check (near (plugin.getPassSeconds(), 1.6), "vitesse bornee a 4 : " + juce::String (plugin.getPassSeconds()));

    setParam (*p, Ids::freezeId, 1.0f);
    run();
    check (plugin.getPassSeconds() <= 0.0f, "gel : " + juce::String (plugin.getPassSeconds()));
    setParam (*p, Ids::freezeId, 0.0f);

    check (textOf (*p, Ids::grainNoteId) == "A0", "note par defaut : " + textOf (*p, Ids::grainNoteId));
    check (std::abs (Ids::noteToHertz (33) - 55.0) < 1.0e-9 && Ids::noteToText (60) == "C3" && Ids::noteToText (12) == "C-1" && Ids::noteToText (84) == "C5",
           "noms de notes");
    check (Ids::textToNote ("A0") == 33 && Ids::textToNote ("c3") == 60 && Ids::textToNote ("F#1") == 42 && Ids::textToNote ("Bb0") == 34
           && Ids::textToNote ("C-1") == 12 && Ids::textToNote ("45") == 45 && Ids::textToNote ("C9") == 84, "lecture des notes");
    check (Ids::stretchToText (8.0f) == "x8.00" && Ids::stretchToText (25.0f) == "x25.0" && Ids::stretchToText (250.0f) == "x250", "texte du facteur");

    p->setPlayHead (nullptr);
}

/** Compte les grains par seconde : tête gelée, grains brefs et séparés (taille 25 %), on compte les départs. */
static double grainsPerSecond (juce::AudioProcessor& p, double sampleRate, FixedTempoPlayHead* playHead, std::vector<int>* onsets = nullptr)
{
    const int blockSize = 256, N = juce::roundToInt (4.0 * sampleRate), S = 4 * blockSize;
    const auto in = makeNoise (2, N, 3);
    juce::AudioBuffer<float> out;

    setParam (p, Ids::stretchId, 0.0f);
    setParam (p, Ids::freezeId, 1.0f);
    setParam (p, Ids::grainSizeId, 25.0f);
    setParam (p, Ids::grainShapeId, 0.0f);
    setParam (p, Ids::limiterId, 0.0f);

    process (p, in, out, blockSize, [&] (int start, juce::MidiBuffer&)
    {
        if (playHead != nullptr)
            playHead->beats = (double) start / sampleRate * playHead->bpm / 60.0;

        if (start >= S) setParam (p, Ids::stretchId, 1.0f);
    });

    const int from = juce::roundToInt (1.0 * sampleRate), to = juce::roundToInt (3.0 * sampleRate);
    int count = 0;

    for (int t = from; t < to; ++t)
    {
        const bool silentBefore = juce::exactlyEqual (out.getSample (0, t - 1), 0.0f) && juce::exactlyEqual (out.getSample (0, t - 2), 0.0f);

        if (silentBefore && ! juce::exactlyEqual (out.getSample (0, t), 0.0f))
        {
            ++count;
            if (onsets != nullptr) onsets->push_back (t);
        }
    }

    setParam (p, Ids::stretchId, 0.0f);
    juce::AudioBuffer<float> flush;
    process (p, makeNoise (2, 2048, 4), flush, blockSize);   // le moteur s'éteint
    return (double) count / 2.0;
}

static void testGrainRate()
{
    std::cout << "- cadence des grains : free, note, sync calee sur la grille de l'hote" << std::endl;

    const double sampleRate = 48000.0;
    auto p = makePlugin (sampleRate, 256);
    FixedTempoPlayHead playHead;
    playHead.bpm = 200.0;
    p->setPlayHead (&playHead);

    setParam (*p, Ids::grainModeId, 0.0f);
    setParam (*p, Ids::grainTimeId, 25.0f);
    const double free = grainsPerSecond (*p, sampleRate, nullptr);
    check (std::abs (free - 40.0) <= 0.5, "free 25 ms : " + juce::String (free) + " grains par seconde au lieu de 40");

    setParam (*p, Ids::grainModeId, 2.0f);
    setParam (*p, Ids::grainNoteId, 33.0f);   // 55 Hz
    const double note = grainsPerSecond (*p, sampleRate, nullptr);
    check (std::abs (note - 55.0) <= 0.5, "note A0 : " + juce::String (note) + " grains par seconde au lieu de 55");

    setParam (*p, Ids::grainNoteId, 57.0f);   // 220 Hz
    const double higher = grainsPerSecond (*p, sampleRate, nullptr);
    check (std::abs (higher - 220.0) <= 1.0, "note A2 : " + juce::String (higher) + " grains par seconde au lieu de 220");

    // 1/128 à 200 BPM : 9,375 ms, soit 106,7 grains par seconde.
    setParam (*p, Ids::grainModeId, 1.0f);
    setParam (*p, Ids::grainDivisionId, 7.0f);
    const double fastest = grainsPerSecond (*p, sampleRate, nullptr);
    check (std::abs (fastest - 106.667) <= 0.6, "1/128 a 200 BPM : " + juce::String (fastest) + " grains par seconde au lieu de 106,7");

    // 1/16 à 200 BPM : un grain toutes les 75 ms (3600 échantillons), sur la grille de l'hôte.
    setParam (*p, Ids::grainDivisionId, 4.0f);
    std::vector<int> onsets;
    const double sync = grainsPerSecond (*p, sampleRate, &playHead, &onsets);
    check (std::abs (sync - 1000.0 / 75.0) <= 0.5, "1/16 a 200 BPM : " + juce::String (sync) + " grains par seconde");

    int offGrid = 0;
    for (const int t : onsets)
        offGrid += (t % 3600 <= 3 || t % 3600 >= 3597) ? 0 : 1;

    check (onsets.size() >= 20 && offGrid == 0, juce::String (offGrid) + " departs de grain hors de la grille sur " + juce::String ((int) onsets.size()));

    // Triolet : 50 ms
    setParam (*p, Ids::grainFeelId, 1.0f);
    const double triplet = grainsPerSecond (*p, sampleRate, &playHead);
    check (std::abs (triplet - 20.0) <= 0.5, "1/16 triolet : " + juce::String (triplet) + " grains par seconde au lieu de 20");

    check (phibres (*p).getEngine().getOutOfRangeReads() == 0, "lectures hors de la zone enregistree");
    p->setPlayHead (nullptr);
}

static void testCaptures()
{
    std::cout << "- recaptures : bouton GRAB, note MIDI a l'echantillon pres, recapture automatique" << std::endl;

    const double sampleRate = 48000.0;
    const int blockSize = 512;
    auto p = makePlugin (sampleRate, blockSize);
    FixedTempoPlayHead playHead;
    playHead.bpm = 120.0;
    p->setPlayHead (&playHead);
    const auto& engine = phibres (*p).getEngine();

    smoothGrains (*p);
    setParam (*p, Ids::stretchId, 1.0f);

    const auto in = makeNoise (2, 20 * blockSize, 5);
    juce::AudioBuffer<float> out;

    process (*p, in, out, blockSize);
    check (engine.getCaptureCount() == 1, "allumage : " + juce::String ((int) engine.getCaptureCount()) + " captures");
    const int firstOrigin = engine.getOrigin();

    // GRAB : la capture part sur le front montant, une seule fois, même si le bouton reste enfoncé.
    process (*p, in, out, blockSize, [&] (int start, juce::MidiBuffer&)
    {
        if (start == 4 * blockSize) setParam (*p, Ids::grabId, 1.0f);
        if (start == 12 * blockSize) setParam (*p, Ids::grabId, 0.0f);
    });
    check (engine.getCaptureCount() == 2, "GRAB : " + juce::String ((int) engine.getCaptureCount()) + " captures au lieu de 2");
    check (engine.getOrigin() - firstOrigin == 24 * blockSize, "GRAB : capture a " + juce::String (engine.getOrigin() - firstOrigin) + " echantillons du debut");

    // Note MIDI au milieu d'un bloc : la capture commence exactement là.
    const int grabOrigin = engine.getOrigin();
    process (*p, in, out, blockSize, [&] (int start, juce::MidiBuffer& midi)
    {
        if (start == 6 * blockSize)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 137);
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 300);   // un relâchement ne déclenche rien
        }
    });
    check (engine.getCaptureCount() == 3, "note MIDI : " + juce::String ((int) engine.getCaptureCount()) + " captures au lieu de 3");
    check (engine.getOrigin() - grabOrigin == 16 * blockSize + 6 * blockSize + 137,
           "note MIDI : capture a " + juce::String (engine.getOrigin() - grabOrigin) + " echantillons au lieu de " + juce::String (22 * blockSize + 137));

    // Recapture automatique : toutes les 1/4 à 120 BPM, soit 24000 échantillons.
    setParam (*p, Ids::autoCaptureId, 1.0f);
    setParam (*p, Ids::autoDivisionId, 6.0f);
    const auto before = engine.getCaptureCount();
    const auto longIn = makeNoise (2, 5 * 48000, 6);
    process (*p, longIn, out, blockSize);
    check (engine.getCaptureCount() - before == 9 || engine.getCaptureCount() - before == 10,
           "recapture automatique : " + juce::String ((int) (engine.getCaptureCount() - before)) + " captures en 5 s au lieu de 10 environ");

    // Éteint, GRAB et les notes ne font rien.
    setParam (*p, Ids::autoCaptureId, 0.0f);
    setParam (*p, Ids::stretchId, 0.0f);
    process (*p, in, out, blockSize);
    const auto whileOff = engine.getCaptureCount();
    process (*p, in, out, blockSize, [&] (int start, juce::MidiBuffer& midi)
    {
        if (start == 2 * blockSize) setParam (*p, Ids::grabId, 1.0f);
        if (start == 3 * blockSize) setParam (*p, Ids::grabId, 0.0f);
        if (start == 5 * blockSize) midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 10);
    });
    check (engine.getCaptureCount() == whileOff && identical (in, out, 0, in.getNumSamples()), "eteint : une capture a eu lieu, ou le son est modifie");
    check (engine.getOutOfRangeReads() == 0 && engine.getStolenGrains() == 0, "moteur : lectures hors zone ou grains coupes");
    p->setPlayHead (nullptr);
}

static void testOutput()
{
    std::cout << "- sortie : mix, filtre, limiteur" << std::endl;

    const double sampleRate = 48000.0;
    const int blockSize = 512, N = 3 * 48000, S = 48000;

    // Mix à 0 % : le son direct, intact, même quand tout tourne.
    {
        auto p = makePlugin (sampleRate, blockSize);
        setParam (*p, Ids::mixId, 0.0f);
        setParam (*p, Ids::stretchId, 1.0f);
        setParam (*p, Ids::filterId, 1.0f);
        setParam (*p, Ids::filterCutoffId, 2000.0f);
        p->prepareToPlay (sampleRate, blockSize);   // le mélange part de la valeur du réglage, comme à l'ouverture d'un projet
        const auto in = makeNoise (2, N, 7);
        juce::AudioBuffer<float> out;
        process (*p, in, out, blockSize);
        check (identical (in, out, 0, N), "mix 0 % : la sortie n'est pas l'entree");
    }

    // Mix à 50 % : la moyenne du son direct et du son traité.
    {
        const auto in = makeNoise (2, N, 8);
        juce::AudioBuffer<float> wet, half;

        for (int pass = 0; pass < 2; ++pass)
        {
            auto p = makePlugin (sampleRate, blockSize);
            smoothGrains (*p);
            setParam (*p, Ids::mixId, pass == 0 ? 100.0f : 50.0f);
            process (*p, in, pass == 0 ? wet : half, blockSize, [&] (int start, juce::MidiBuffer&) { if (start >= S) setParam (*p, Ids::stretchId, 1.0f); });
        }

        float worst = 0.0f;
        for (int t = S; t < N; ++t)
            worst = juce::jmax (worst, std::abs (half.getSample (0, t) - 0.5f * (in.getSample (0, t) + wet.getSample (0, t))));

        check (worst < 1.0e-5f, "mix 50 % : ecart " + juce::String (worst));
    }

    // Limiteur : il tient la sortie sous 0 dBFS, filtre allumé ou non ; éteint, il laisse passer.
    for (const bool limiter : { true, false })
    {
        for (const bool filter : { false, true })
        {
            auto p = makePlugin (sampleRate, blockSize);
            setParam (*p, Ids::limiterId, limiter ? 1.0f : 0.0f);
            setParam (*p, Ids::filterId, filter ? 1.0f : 0.0f);
            setParam (*p, Ids::filterTypeId, 2.0f);
            setParam (*p, Ids::filterCutoffId, 8000.0f);
            setParam (*p, Ids::scatterId, 1.0f);
            setParam (*p, Ids::widthId, 100.0f);
            setParam (*p, Ids::stretchId, 1.0f);
            const auto in = makeNoise (2, N, 9, 2.0f);   // entrée bien au-dessus du plafond
            juce::AudioBuffer<float> out;
            process (*p, in, out, blockSize);
            const float peak = out.getMagnitude (0, N);   // dès le premier échantillon
            check (limiter ? peak <= 1.0001f : peak > 1.2f,
                   juce::String ("limiteur ") + (limiter ? "allume" : "eteint") + (filter ? ", filtre allume" : ", filtre eteint") + " : crete " + juce::String (peak));
            check (allFinite (out), "sortie non finie");
        }
    }

    // Filtre : un passe-bas à 500 Hz retire l'essentiel d'un bruit blanc.
    {
        const auto in = makeNoise (2, N, 10);
        juce::AudioBuffer<float> open, closed;

        for (int pass = 0; pass < 2; ++pass)
        {
            auto p = makePlugin (sampleRate, blockSize);
            smoothGrains (*p);
            setParam (*p, Ids::stretchId, 1.0f);
            setParam (*p, Ids::filterId, pass == 0 ? 0.0f : 1.0f);
            setParam (*p, Ids::filterTypeId, 2.0f);
            setParam (*p, Ids::filterSlopeId, 1.0f);
            setParam (*p, Ids::filterCutoffId, 500.0f);
            setParam (*p, Ids::filterResonanceId, 0.0f);
            process (*p, in, pass == 0 ? open : closed, blockSize);
        }

        const float ratio = closed.getRMSLevel (0, S, N - S) / open.getRMSLevel (0, S, N - S);
        check (ratio > 0.08f && ratio < 0.25f, "passe-bas 500 Hz : niveau relatif " + juce::String (ratio));
    }
}

static void testParametersAndState()
{
    std::cout << "- parametres et sauvegarde (reglages et capture)" << std::endl;

    const double sampleRate = 48000.0;
    const int blockSize = 512;

    // Liste des paramètres : les identifiants sont figés, les projets enregistrés en dépendent.
    {
        auto p = makePlugin (sampleRate, blockSize);
        const juce::StringArray expected { "stretch", "lengthMode", "lengthTime", "lengthDivision", "lengthFeel", "grab", "autoCapture", "autoDivision", "position",
                                           "stretchMode", "stretchFactor", "stretchDivision", "stretchFeel", "freeze", "reverse", "pitch", "pitchMode",
                                           "grainMode", "grainTime", "grainDivision", "grainFeel", "grainNote", "grainSize", "grainShape", "grainTilt", "flip",
                                           "scatter", "spray", "detune", "detuneMode", "flipChance", "width",
                                           "filter", "filterType", "filterCutoff", "filterResonance", "filterSlope", "limiter", "mix" };
        juce::StringArray found;

        for (auto* parameter : p->getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
                found.add (withId->paramID);

        check (found == expected, "liste des parametres : " + found.joinIntoString (","));
        check (p->acceptsMidi() && ! p->producesMidi() && p->getName() == "Phibres", "identite du plugin");

        // Valeurs par défaut lisibles
        check (textOf (*p, Ids::lengthDivisionId) == "1/1" && textOf (*p, Ids::stretchFactorId) == "x8.00" && textOf (*p, Ids::grainTimeId) == "60.0 ms"
               && textOf (*p, Ids::grainSizeId) == "200 %" && textOf (*p, Ids::sprayId) == "20.0 ms" && textOf (*p, Ids::autoDivisionId) == "4/1"
               && textOf (*p, Ids::limiterId) == "On" && textOf (*p, Ids::stretchId) == "Off", "valeurs par defaut");

        // Chaque texte affiché se relit en la même valeur.
        for (auto* parameter : p->getParameters())
        {
            for (const float normalised : { 0.0f, 0.31f, 0.5f, 0.77f, 1.0f })
            {
                parameter->setValue (normalised);
                const auto text = parameter->getText (parameter->getValue(), 64);
                const auto back = parameter->getText (parameter->getValueForText (text), 64);
                check (back == text, "texte non relu : " + parameter->getName (64) + " = " + text + " -> " + back);
            }
        }
    }

    // Réglages : tout ce qui est enregistré est retrouvé.
    juce::MemoryBlock settingsOnly;
    {
        auto a = makePlugin (sampleRate, blockSize);
        juce::Random rng (11);
        juce::StringArray values;

        for (auto* parameter : a->getParameters())
            parameter->setValueNotifyingHost (rng.nextFloat());

        setParam (*a, Ids::stretchId, 0.0f);   // éteint : pas de capture dans la sauvegarde

        for (auto* parameter : a->getParameters())
            values.add (parameter->getCurrentValueAsText());

        check (values.contains ("Sync") && values.contains ("On") && values.contains ("Off"), "tirage des reglages : trop uniforme");
        a->getStateInformation (settingsOnly);
        check (settingsOnly.getSize() < 8000, "sauvegarde sans capture : " + juce::String ((int) settingsOnly.getSize()) + " octets");

        auto b = makePlugin (sampleRate, blockSize);
        b->setStateInformation (settingsOnly.getData(), (int) settingsOnly.getSize());

        for (int i = 0; i < b->getParameters().size(); ++i)
            check (b->getParameters()[i]->getCurrentValueAsText() == values[i],
                   "reglage recharge : " + b->getParameters()[i]->getName (64) + " = " + b->getParameters()[i]->getCurrentValueAsText() + " au lieu de " + values[i]);
    }

    // Capture : enregistrée avec le projet, retrouvée à la réouverture, sans recapturer.
    {
        auto a = makePlugin (sampleRate, blockSize);
        smoothGrains (*a);
        setParam (*a, Ids::lengthModeId, 0.0f);
        setParam (*a, Ids::lengthTimeId, 500.0f);
        setParam (*a, Ids::stretchFactorId, 4.0f);

        const int S = 100 * blockSize;                              // plus d'une seconde de son avant la capture
        const auto in = makeNoise (2, S + 3 * 48000, 12, 1.6f);     // au-dessus de 0 dBFS : rien ne doit être écrêté
        juce::AudioBuffer<float> out;
        process (*a, in, out, blockSize, [&] (int start, juce::MidiBuffer&) { if (start >= S) setParam (*a, Ids::stretchId, 1.0f); });

        juce::MemoryBlock state;
        a->getStateInformation (state);

        // 0,5 s d'historique + 0,5 s de boucle + 2 s de suite, en stéréo sur 24 bits : 864 ko.
        const int expectedBytes = 3 * 48000 * 2 * 3;
        check (std::abs ((int) state.getSize() - expectedBytes) < 20000, "taille de la sauvegarde : " + juce::String ((int) state.getSize()) + " octets au lieu de " + juce::String (expectedBytes) + " environ");

        PhibresAudioProcessor::SavedTape original;
        check (phibres (*a).snapshotTape (original) && original.preRoll == 24000 && original.audio.getNumSamples() == 144000, "capture a sauvegarder : dimensions inattendues");

        float worstInput = 0.0f;
        for (int i = 0; i < original.audio.getNumSamples(); ++i)
            worstInput = juce::jmax (worstInput, std::abs (original.audio.getSample (1, i) - in.getSample (1, S - 24000 + i)));
        check (juce::exactlyEqual (worstInput, 0.0f), "la capture n'est pas le son entre a ce moment-la");

        for (const double newRate : { 48000.0, 96000.0, 44100.0 })
        {
            auto b = makePlugin (newRate, blockSize);
            b->setStateInformation (state.getData(), (int) state.getSize());
            check (textOf (*b, Ids::stretchId) == "On" && textOf (*b, Ids::stretchFactorId) == "x4.00", "reglages non recharges");

            // Sauvegarder aussitôt, avant d'avoir traité le moindre son : la capture ne doit pas se perdre.
            juce::MemoryBlock again;
            b->getStateInformation (again);
            check (again.getSize() == state.getSize(), "resauvegarde immediate : " + juce::String ((int) again.getSize()) + " octets au lieu de " + juce::String ((int) state.getSize()));

            // Entrée silencieuse : tout ce qui sort vient de la capture rechargée.
            juce::AudioBuffer<float> silence (2, 16 * blockSize), restored;
            silence.clear();
            process (*b, silence, restored, blockSize);

            const auto& engine = phibres (*b).getEngine();
            GrainStretcher::View view;
            check (engine.getView (view) && view.running && engine.getCaptureCount() == 1, "la lecture ne reprend pas sur la capture rechargee");
            check (restored.getRMSLevel (0, 0, blockSize) > 0.1f, "pas de son des le premier bloc : " + juce::String (restored.getRMSLevel (0, 0, blockSize)));

            const double ratio = newRate / 48000.0;
            check (std::abs (view.origin - juce::roundToInt (24000 * ratio)) <= 1 && std::abs (view.end - juce::roundToInt (144000 * ratio)) <= 1,
                   juce::String (newRate) + " Hz : bande rechargee de " + juce::String (view.end) + " echantillons, origine " + juce::String (view.origin));

            if (juce::exactlyEqual (newRate, 48000.0))
            {
                float worst = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 144000; ++i)
                        worst = juce::jmax (worst, std::abs (engine.getTapeData (view.tape, ch)[i] - original.audio.getSample (ch, i)));

                check (worst < 4.0e-7f, "capture rechargee : ecart " + juce::String (worst) + " (24 bits attendus)");
            }
            else
            {
                // Autre fréquence d'échantillonnage : même son, rééchantillonné. On compare des moyennes glissantes (le grave),
                // où l'interpolation est précise.
                float worst = 0.0f;

                for (int i = 0; i < 2000; ++i)
                {
                    float before = 0.0f, after = 0.0f;
                    const int width = juce::roundToInt (64 * ratio);
                    const int start = juce::roundToInt ((60000 + i * 4) * ratio);

                    for (int k = 0; k < 64; ++k)
                        before += original.audio.getSample (0, 60000 + i * 4 + k);

                    for (int k = 0; k < width; ++k)
                        after += engine.getTapeData (view.tape, 0)[start + k];

                    worst = juce::jmax (worst, std::abs (after / (float) width - before / 64.0f));
                }

                check (worst < 0.05f, juce::String (newRate) + " Hz : la capture reechantillonnee differe de " + juce::String (worst));
            }

            // L'hôte relance le moteur audio (prepareToPlay) : la capture survit.
            b->prepareToPlay (newRate, 256);
            process (*b, silence, restored, 256);
            check (engine.getView (view) && view.running && restored.getRMSLevel (0, 0, 256) > 0.1f, juce::String (newRate) + " Hz : capture perdue apres prepareToPlay");
            check (engine.getOutOfRangeReads() == 0, "lectures hors de la zone enregistree");
        }

        // Projet rouvert avec STRETCH éteint entre-temps : on ne rejoue pas une vieille capture.
        auto c = makePlugin (sampleRate, blockSize);
        c->setStateInformation (state.getData(), (int) state.getSize());
        setParam (*c, Ids::stretchId, 0.0f);
        juce::AudioBuffer<float> silence (2, 8 * blockSize), result;
        silence.clear();
        process (*c, silence, result, blockSize);
        check (result.getMagnitude (0, result.getNumSamples()) <= 0.0f, "eteint, la capture rechargee s'entend");

        // États abîmés : rien ne casse, et les réglages passent même si la capture est tronquée.
        auto d = makePlugin (sampleRate, blockSize);
        const char garbage[] = "pas un etat valide";
        d->setStateInformation (garbage, (int) sizeof (garbage));
        d->setStateInformation (nullptr, 0);
        d->setStateInformation (state.getData(), 6);
        d->setStateInformation (state.getData(), (int) state.getSize() / 2);
        check (textOf (*d, Ids::stretchFactorId) == "x4.00", "capture tronquee : les reglages auraient du passer");
        process (*d, silence, result, blockSize);
        check (allFinite (result), "capture tronquee : sortie non finie");

        juce::MemoryBlock corrupted (state);
        juce::Random rng (13);
        for (int i = 0; i < 400; ++i)
            static_cast<char*> (corrupted.getData())[rng.nextInt ((int) juce::jmin ((size_t) 6000, corrupted.getSize()))] = (char) rng.nextInt (256);
        d->setStateInformation (corrupted.getData(), (int) corrupted.getSize());
        process (*d, silence, result, blockSize);
        check (allFinite (result), "etat corrompu : sortie non finie");

        // Des réglages seuls (sans capture) se chargent aussi.
        d->setStateInformation (settingsOnly.getData(), (int) settingsOnly.getSize());
        check (textOf (*d, Ids::stretchId) == "Off", "reglages seuls : non charges");
    }

    // Bypass de l'hôte : on repart d'un état propre, une nouvelle capture démarre à la réactivation.
    {
        auto p = makePlugin (sampleRate, blockSize);
        setParam (*p, Ids::stretchId, 1.0f);
        const auto in = makeNoise (2, 20 * blockSize, 14);
        juce::AudioBuffer<float> out;
        process (*p, in, out, blockSize);
        const auto& engine = phibres (*p).getEngine();
        check (engine.isRunning(), "le moteur devrait tourner");

        juce::AudioBuffer<float> block;
        block.makeCopyOf (in);
        juce::MidiBuffer midi;
        p->processBlockBypassed (block, midi);
        check (! engine.isRunning() && identical (in, block, 0, in.getNumSamples()), "bypass : le moteur tourne encore, ou le son est modifie");

        process (*p, in, out, blockSize);
        check (engine.isRunning() && engine.getCaptureCount() == 1, "apres le bypass : pas de nouvelle capture");

        // reset() : les grains s'arrêtent, la capture reste.
        p->reset();
        check (engine.isRunning() && engine.getNumActiveGrains() == 0, "reset : etat inattendu");
        process (*p, in, out, blockSize);
        check (engine.getCaptureCount() == 1 && engine.getNumActiveGrains() > 0, "reset : la capture aurait du rester");
    }
}

static void testEditor (const juce::File& outputDir)
{
    std::cout << "- interface" << std::endl;

    const double sampleRate = 48000.0;
    auto p = makePlugin (sampleRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditorAndMakeActive());
    check (editor != nullptr, "pas d'interface");

    if (editor == nullptr)
        return;

    // Les contrôles sont dans la façade, mise à l'échelle par l'éditeur
    juce::Component* ui = &dynamic_cast<PhibresAudioProcessorEditor&> (*editor).getFacade();

    auto snapshot = [&] (const juce::String& name)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (120);

        if (outputDir == juce::File())
            return;

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        const auto file = outputDir.getChildFile (name + ".png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        check (stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream), "ecriture de " + file.getFullPathName());
    };

    check (editor->getWidth() == 1218 && editor->getHeight() == 642, "taille d'interface : " + juce::String (editor->getWidth()) + " x " + juce::String (editor->getHeight()));

    const auto knob = [&ui] (const char* id) { return dynamic_cast<juce::Slider*> (ui->findChildWithID (id)); };
    const auto button = [&ui] (const juce::String& id) { return dynamic_cast<juce::TextButton*> (ui->findChildWithID (id)); };

    const auto readouts = [&ui]
    {
        juce::StringArray texts;
        for (int column = 0; column < 5; ++column)
            if (auto* label = dynamic_cast<juce::Label*> (ui->findChildWithID ("value/" + juce::String (column))))
                texts.add (label->getText());
        return texts.joinIntoString (" | ");
    };

    snapshot ("ui_default");
    check (readouts() == "1/1 | x8.00 | 60.0 ms | 20.0 ms | 20 Hz", "afficheurs par defaut : " + readouts());

    bool allFound = true;
    for (const char* id : { Ids::lengthTimeId, Ids::lengthDivisionId, Ids::stretchFactorId, Ids::stretchDivisionId, Ids::grainTimeId, Ids::grainDivisionId,
                            Ids::grainNoteId, Ids::sprayId, Ids::filterCutoffId, Ids::positionId, Ids::autoDivisionId, Ids::pitchId, Ids::grainSizeId,
                            Ids::grainShapeId, Ids::grainTiltId, Ids::detuneId, Ids::flipChanceId, Ids::widthId, Ids::filterResonanceId, Ids::mixId })
        allFound = allFound && knob (id) != nullptr;

    for (const char* id : { Ids::lengthModeId, Ids::stretchModeId, Ids::grainModeId, Ids::detuneModeId, Ids::filterTypeId, Ids::filterSlopeId, Ids::pitchModeId })
        allFound = allFound && button (juce::String (id) + "/0") != nullptr && button (juce::String (id) + "/1") != nullptr;

    for (const char* id : { Ids::grainModeId, Ids::detuneModeId, Ids::filterTypeId })
        allFound = allFound && button (juce::String (id) + "/2") != nullptr;

    for (const char* id : { Ids::lengthFeelId, Ids::stretchFeelId, Ids::grainFeelId })
        allFound = allFound && button (juce::String (id) + "/1") != nullptr && button (juce::String (id) + "/2") != nullptr;

    for (const char* id : { Ids::stretchId, Ids::freezeId, Ids::flipId, Ids::scatterId, Ids::filterId, Ids::autoCaptureId, Ids::grabId, Ids::reverseId, Ids::limiterId })
        allFound = allFound && button (id) != nullptr;

    auto* tape = dynamic_cast<TapeDisplay*> (ui->findChildWithID ("tape"));
    allFound = allFound && tape != nullptr;
    check (allFound, "controles introuvables dans l'interface");

    if (allFound)
    {
        auto& apvts = phibres (*p).apvts;

        // Tous les contrôles tiennent dans la façade et aucun ne chevauche un autre (hors potards superposés d'un même module).
        juce::Array<juce::Component*> controls;
        for (auto* child : ui->getChildren())
            if (child->isVisible())
                controls.add (child);

        for (auto* control : controls)
            check (ui->getLocalBounds().reduced (12).contains (control->getBounds()), "controle hors de la facade : " + control->getComponentID());

        for (int i = 0; i < controls.size(); ++i)
            for (int j = i + 1; j < controls.size(); ++j)
                check (! controls[i]->getBounds().intersects (controls[j]->getBounds()),
                       "controles qui se chevauchent : " + controls[i]->getComponentID() + " / " + controls[j]->getComponentID());

        // Les grands boutons sont des bascules reliées à leur paramètre.
        for (const char* id : { Ids::stretchId, Ids::freezeId, Ids::flipId, Ids::scatterId, Ids::filterId })
        {
            button (id)->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            check (apvts.getParameter (id)->getValue() > 0.5f && button (id)->getToggleState(), juce::String ("un clic devrait allumer ") + id);
        }

        setParam (*p, Ids::freezeId, 0.0f);

        // Un son passe : la bande se remplit et la tête avance.
        check (! tape->isShowingCapture(), "bande : une capture est affichee avant tout son");
        const auto in = makeNoise (2, 24000, 15);
        juce::AudioBuffer<float> out;
        process (*p, in, out, 512);
        tape->refresh();
        check (tape->isShowingCapture() && std::abs (tape->getShownRecorded() - 0.25f) < 0.02f,
               "bande : " + juce::String (tape->getShownRecorded()) + " enregistre au lieu de 0,25 (0,5 s sur une boucle de 2 s)");
        check (std::abs (tape->getShownPlayhead() - 0.25f / 8.0f) < 0.005f, "bande : tete a " + juce::String (tape->getShownPlayhead()));

        // Réglages variés, en sync
        setParam (*p, Ids::lengthDivisionId, 5.0f);     // 1/4
        setParam (*p, Ids::stretchModeId, 1.0f);
        setParam (*p, Ids::stretchDivisionId, 4.0f);    // sur 4/1
        setParam (*p, Ids::grainModeId, 1.0f);
        setParam (*p, Ids::grainDivisionId, 6.0f);      // 1/64
        setParam (*p, Ids::grainSizeId, 300.0f);
        setParam (*p, Ids::grainShapeId, 60.0f);
        setParam (*p, Ids::grainTiltId, -50.0f);
        setParam (*p, Ids::sprayId, 150.0f);
        setParam (*p, Ids::detuneId, 7.0f);
        setParam (*p, Ids::flipChanceId, 40.0f);
        setParam (*p, Ids::widthId, 100.0f);
        setParam (*p, Ids::filterCutoffId, 320.0f);
        setParam (*p, Ids::filterResonanceId, 60.0f);
        setParam (*p, Ids::pitchId, 3.0f);
        setParam (*p, Ids::positionId, 25.0f);
        setParam (*p, Ids::autoCaptureId, 1.0f);
        process (*p, makeNoise (2, 48000, 16), out, 512);
        tape->refresh();
        snapshot ("ui_all_on");
        check (readouts() == "1/4 | 4/1 | 1/64 | 150 ms | 320 Hz", "afficheurs en sync : " + readouts());
        check (knob (Ids::pitchId)->getTextFromValue (knob (Ids::pitchId)->getValue()) == "+3 st", "valeur de pitch : " + knob (Ids::pitchId)->getTextFromValue (knob (Ids::pitchId)->getValue()));
        check (knob (Ids::lengthDivisionId)->isVisible() && ! knob (Ids::lengthTimeId)->isVisible()
               && knob (Ids::stretchDivisionId)->isVisible() && ! knob (Ids::stretchFactorId)->isVisible()
               && knob (Ids::grainDivisionId)->isVisible() && ! knob (Ids::grainTimeId)->isVisible() && ! knob (Ids::grainNoteId)->isVisible(),
               "en sync : mauvais potards visibles");

        // Triolet et pointé : un clic sur T, l'afficheur le dit ; un second clic revient à la normale.
        button (juce::String (Ids::lengthFeelId) + "/1")->onClick();
        check (textOf (*p, Ids::lengthFeelId) == "Triplet", "le clic sur T ne change pas le parametre");
        button (juce::String (Ids::grainFeelId) + "/2")->onClick();
        check (readouts() == "1/4 T | 4/1 | 1/64 D | 150 ms | 320 Hz", "afficheurs en triolet / pointe : " + readouts());
        button (juce::String (Ids::lengthFeelId) + "/1")->onClick();
        button (juce::String (Ids::grainFeelId) + "/2")->onClick();
        check (readouts() == "1/4 | 4/1 | 1/64 | 150 ms | 320 Hz", "retour a la division normale : " + readouts());

        // GRAB : une touche. Le paramètre vaut 1 tant qu'elle est enfoncée, puis revient à 0.
        button (Ids::grabId)->setState (juce::Button::buttonDown);
        check (apvts.getParameter (Ids::grabId)->getValue() > 0.5f, "GRAB enfonce : le parametre ne monte pas");
        button (Ids::grabId)->setState (juce::Button::buttonNormal);
        check (apvts.getParameter (Ids::grabId)->getValue() < 0.5f, "GRAB relache : le parametre ne redescend pas");

        // Pitch : en mode libre la valeur reste au centième ; en mode demi-tons, le potard se pose sur le demi-ton quand on le lâche.
        button (juce::String (Ids::pitchModeId) + "/1")->onClick();
        check (textOf (*p, Ids::pitchModeId) == "Free", "le clic sur FREE (pitch) ne change pas le parametre");
        setParam (*p, Ids::pitchId, 3.37f);
        knob (Ids::pitchId)->onDragEnd();
        check (textOf (*p, Ids::pitchId) == "+3.37 st", "pitch libre : " + textOf (*p, Ids::pitchId));
        button (juce::String (Ids::pitchModeId) + "/0")->onClick();
        knob (Ids::pitchId)->onDragEnd();
        check (textOf (*p, Ids::pitchId) == "+3 st", "pitch en demi-tons : " + textOf (*p, Ids::pitchId));

        // Grains accordés sur une note : l'afficheur montre la note, on peut la taper au clavier.
        button (juce::String (Ids::grainModeId) + "/2")->onClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (textOf (*p, Ids::grainModeId) == "Note" && knob (Ids::grainNoteId)->isVisible() && ! knob (Ids::grainDivisionId)->isVisible(), "mode note : mauvais potard visible");
        check (readouts() == "1/4 | 4/1 | A0 | 150 ms | 320 Hz", "afficheurs en mode note : " + readouts());
        check (! button (juce::String (Ids::grainFeelId) + "/1")->isEnabled() && button (juce::String (Ids::lengthFeelId) + "/1")->isEnabled(),
               "triolet / pointe : hors service hors du mode sync seulement");

        if (auto* label = dynamic_cast<juce::Label*> (ui->findChildWithID ("value/2")))
            label->setText ("F#1", juce::sendNotificationSync);

        check (textOf (*p, Ids::grainNoteId) == "F#1", "saisie d'une note : " + textOf (*p, Ids::grainNoteId));
        setParam (*p, Ids::reverseId, 1.0f);
        setParam (*p, Ids::detuneModeId, 2.0f);
        setParam (*p, Ids::freezeId, 1.0f);
        process (*p, makeNoise (2, 4096, 17), out, 512);
        snapshot ("ui_note_frozen");

        // Tout en FREE, filtre en passe-bande 24 dB.
        setParam (*p, Ids::freezeId, 0.0f);
        button (juce::String (Ids::lengthModeId) + "/0")->onClick();
        button (juce::String (Ids::stretchModeId) + "/0")->onClick();
        button (juce::String (Ids::grainModeId) + "/0")->onClick();
        button (juce::String (Ids::filterTypeId) + "/1")->onClick();
        button (juce::String (Ids::filterSlopeId) + "/1")->onClick();
        button (juce::String (Ids::detuneModeId) + "/1")->onClick();
        setParam (*p, Ids::lengthTimeId, 1500.0f);
        setParam (*p, Ids::stretchFactorId, 250.0f);
        setParam (*p, Ids::grainTimeId, 12.5f);
        setParam (*p, Ids::filterCutoffId, 2500.0f);
        setParam (*p, Ids::pitchModeId, 1.0f);
        setParam (*p, Ids::pitchId, -5.25f);
        setParam (*p, Ids::mixId, 70.0f);
        setParam (*p, Ids::grainSizeId, 50.0f);
        setParam (*p, Ids::grainShapeId, 10.0f);
        setParam (*p, Ids::grainTiltId, 0.0f);
        setParam (*p, Ids::scatterId, 0.0f);
        process (*p, makeNoise (2, 4096, 18), out, 512);
        snapshot ("ui_free");
        check (textOf (*p, Ids::filterTypeId) == "Band Pass" && textOf (*p, Ids::filterSlopeId) == "24 dB" && textOf (*p, Ids::detuneModeId) == "Semitones",
               "les clics sur les interrupteurs ne changent pas les parametres");
        check (readouts() == "1.50 s | x250 | 12.5 ms | 150 ms | 2.50 kHz", "afficheurs en free : " + readouts());
        check (knob (Ids::lengthTimeId)->isVisible() && knob (Ids::stretchFactorId)->isVisible() && knob (Ids::grainTimeId)->isVisible(), "tout en free : mauvais potards visibles");

        // 1,5 s sur une plage 20 ms - 30 s : le potard est à gauche du milieu (long = gauche).
        const double position = knob (Ids::lengthTimeId)->valueToProportionOfLength (knob (Ids::lengthTimeId)->getValue());
        check (position > 0.3 && position < 0.5, "position du potard de longueur : " + juce::String (position));

        // Saisie au clavier dans les afficheurs
        if (auto* label = dynamic_cast<juce::Label*> (ui->findChildWithID ("value/4")))
            label->setText ("800", juce::sendNotificationSync);
        if (auto* label = dynamic_cast<juce::Label*> (ui->findChildWithID ("value/1")))
            label->setText ("16", juce::sendNotificationSync);
        if (auto* label = dynamic_cast<juce::Label*> (ui->findChildWithID ("value/0")))
            label->setText ("2 s", juce::sendNotificationSync);

        check (textOf (*p, Ids::filterCutoffId) == "800 Hz" && textOf (*p, Ids::stretchFactorId) == "x16.0" && textOf (*p, Ids::lengthTimeId) == "2.00 s",
               "saisie au clavier : " + textOf (*p, Ids::filterCutoffId) + " / " + textOf (*p, Ids::stretchFactorId) + " / " + textOf (*p, Ids::lengthTimeId));

        // L'hôte relance le moteur audio pendant que l'interface est ouverte : rien ne casse.
        p->prepareToPlay (96000.0, 256);
        tape->refresh();
        process (*p, makeNoise (2, 8192, 19), out, 256);
        tape->refresh();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (80);
    }

    p->editorBeingDeleted (editor.get());   // ce que fait un hôte avant de détruire l'interface
    editor.reset();
}

#include "PresetTests.h"

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
        for (int blockSize : { 64, 512, 441 })
            testPassthroughAndLoop (sampleRate, blockSize, 2);

    testPassthroughAndLoop (48000.0, 256, 1);
    testTempoAndModes();
    testGrainRate();
    testCaptures();
    testOutput();
    testParametersAndState();
    testEditor (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[1])) : juce::File());
    testPresetsAndWindowSize<PhibresAudioProcessor, PhibresAudioProcessorEditor, PhibresFacade> ({ "grab" },
        argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[1])) : juce::File());

    std::cout << (failures == 0 ? "TOUS LES TESTS PASSENT" : juce::String (failures) + " ECHEC(S)") << std::endl;
    return failures == 0 ? 0 : 1;
}
