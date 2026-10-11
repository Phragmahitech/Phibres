#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Sauvegarde : [marque] [taille du XML] [XML des paramètres] [taille de la capture] [capture]
    constexpr int containerMagic = (int) juce::ByteOrder::makeInt ('P', 'H', 'B', '1');
    constexpr int tapeMagic = (int) juce::ByteOrder::makeInt ('P', 'H', 'T', 'P');
    constexpr int tapeVersion = 1;

    // Ce qu'on garde de la bande autour de la boucle : assez pour que les grains sonnent pareil à la réouverture.
    constexpr double savedPreRollSeconds = 0.5;
    constexpr double savedPostRollSeconds = 2.0;

    juce::String timeToText (float ms, int)
    {
        if (ms >= 1000.0f)
            return juce::String (ms / 1000.0f, 2) + " s";

        if (ms < 99.95f)
            return juce::String (ms, 1) + " ms";

        return juce::String (juce::roundToInt (ms)) + " ms";
    }

    float textToNumber (const juce::String& text)
    {
        return text.retainCharacters ("-0123456789.,").replaceCharacter (',', '.').getFloatValue();
    }

    float textToTime (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        const auto value = t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue();

        // "1.5 s" -> 1500 ms ; "250" ou "250 ms" -> 250 ms
        return (t.endsWithChar ('s') && ! t.endsWith ("ms")) ? value * 1000.0f : value;
    }

    // Plage de temps exponentielle et INVERSÉE : 0 = le plus long, 1 = le plus court.
    // Tourner le potard à droite (ou monter la valeur en automation / MIDI) raccourcit.
    juce::NormalisableRange<float> makeReversedTimeRange (float shortestMs, float longestMs)
    {
        return { shortestMs, longestMs,
                 [] (float start, float end, float normalised)
                 {
                     return end * std::pow (start / end, juce::jlimit (0.0f, 1.0f, normalised));
                 },
                 [] (float start, float end, float value)
                 {
                     return juce::jlimit (0.0f, 1.0f, std::log (juce::jlimit (start, end, value) / end) / std::log (start / end));
                 },
                 [] (float start, float end, float value)
                 {
                     const float step = value < 100.0f ? 0.1f : 1.0f;   // plus fin sur les durées très courtes
                     return juce::jlimit (start, end, std::round (value / step) * step);
                 } };
    }

    /** Plage logarithmique croissante, arrondie à trois chiffres utiles environ. */
    juce::NormalisableRange<float> makeLogRange (float lowest, float highest)
    {
        return { lowest, highest,
                 [] (float start, float end, float normalised)
                 {
                     return start * std::pow (end / start, juce::jlimit (0.0f, 1.0f, normalised));
                 },
                 [] (float start, float end, float value)
                 {
                     return juce::jlimit (0.0f, 1.0f, std::log (juce::jlimit (start, end, value) / start) / std::log (end / start));
                 },
                 [] (float start, float end, float value)
                 {
                     const float step = value < 10.0f ? 0.01f : value < 100.0f ? 0.1f : value < 1000.0f ? 1.0f : 10.0f;
                     return juce::jlimit (start, end, std::round (value / step) * step);
                 } };
    }

    juce::String frequencyToText (float hertz, int)
    {
        if (hertz >= 1000.0f)
            return juce::String (hertz / 1000.0f, 2) + " kHz";

        return juce::String (juce::roundToInt (hertz)) + " Hz";
    }

    float textToFrequency (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        const auto value = t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue();

        // "1.2k" ou "1.2 kHz" -> 1200 Hz ; "250" ou "250 Hz" -> 250 Hz
        return t.containsChar ('k') ? value * 1000.0f : value;
    }

    std::unique_ptr<juce::AudioParameterFloat> makePercentParameter (const char* id, const char* name, float defaultPercent,
                                                                     float lowest = 0.0f, float highest = 100.0f)
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name,
                                                            juce::NormalisableRange<float> (lowest, highest, 1.0f), defaultPercent,
                                                            juce::AudioParameterFloatAttributes()
                                                                .withStringFromValueFunction ([] (float value, int) { return juce::String (juce::roundToInt (value)) + " %"; })
                                                                .withValueFromStringFunction (textToNumber));
    }

    std::unique_ptr<juce::AudioParameterBool> makeSwitch (const char* id, const char* name, bool defaultValue)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, defaultValue);
    }

    std::unique_ptr<juce::AudioParameterChoice> makeChoice (const char* id, const char* name, const juce::StringArray& choices, int defaultIndex)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, defaultIndex);
    }

    std::unique_ptr<juce::AudioParameterFloat> makeTimeParameter (const char* id, const char* name,
                                                                  float shortestMs, float longestMs, float defaultMs)
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name,
                                                            makeReversedTimeRange (shortestMs, longestMs), defaultMs,
                                                            juce::AudioParameterFloatAttributes()
                                                                .withStringFromValueFunction (timeToText)
                                                                .withValueFromStringFunction (textToTime));
    }

    double beatsAt (const double* table, int size, int index)
    {
        return table[juce::jlimit (0, size - 1, index)];
    }

    float hermite (float ym1, float y0, float y1, float y2, float t)
    {
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }
}

//==============================================================================
const juce::StringArray& PhibresAudioProcessor::getFeelNames()
{
    static const juce::StringArray names { "Straight", "Triplet", "Dotted" };
    return names;
}

double PhibresAudioProcessor::getFeelFactor (int index)
{
    static constexpr double factors[] = { 1.0, 2.0 / 3.0, 1.5 };
    return beatsAt (factors, (int) std::size (factors), index);
}

const juce::StringArray& PhibresAudioProcessor::getLengthDivisionNames()
{
    static const juce::StringArray names { "8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
    return names;
}

double PhibresAudioProcessor::getLengthDivisionInBeats (int index)
{
    static constexpr double beats[] = { 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
    return beatsAt (beats, (int) std::size (beats), index);
}

const juce::StringArray& PhibresAudioProcessor::getAutoDivisionNames()
{
    static const juce::StringArray names { "16/1", "8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16" };
    return names;
}

double PhibresAudioProcessor::getAutoDivisionInBeats (int index)
{
    static constexpr double beats[] = { 64.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25 };
    return beatsAt (beats, (int) std::size (beats), index);
}

const juce::StringArray& PhibresAudioProcessor::getStretchDivisionNames()
{
    static const juce::StringArray names { "1/4", "1/2", "1/1", "2/1", "4/1", "8/1", "16/1", "32/1", "64/1" };
    return names;
}

double PhibresAudioProcessor::getStretchDivisionInBeats (int index)
{
    static constexpr double beats[] = { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0 };
    return beatsAt (beats, (int) std::size (beats), index);
}

const juce::StringArray& PhibresAudioProcessor::getGrainDivisionNames()
{
    static const juce::StringArray names { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128" };
    return names;
}

double PhibresAudioProcessor::getGrainDivisionInBeats (int index)
{
    static constexpr double beats[] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125 };
    return beatsAt (beats, (int) std::size (beats), index);
}

juce::String PhibresAudioProcessor::noteToText (int midiNote)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int note = juce::jlimit (0, 127, midiNote);
    return juce::String (names[note % 12]) + juce::String (note / 12 - 2);
}

int PhibresAudioProcessor::textToNote (const juce::String& text)
{
    const auto t = text.trim().toUpperCase();

    if (t.isEmpty())
        return 33;

    static const int semitones[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    const auto letter = t[0];

    if (letter < 'A' || letter > 'G')
        return juce::jlimit (lowestNote, highestNote, t.getIntValue());   // un numéro de note MIDI

    int index = 1, note = semitones[letter - 'A'];

    if (t[index] == '#')       { ++note; ++index; }
    else if (t[index] == 'B')  { --note; ++index; }

    const auto rest = t.substring (index).retainCharacters ("-0123456789");
    const int octave = rest.isEmpty() ? 0 : rest.getIntValue();
    return juce::jlimit (lowestNote, highestNote, note + 12 * (octave + 2));
}

double PhibresAudioProcessor::noteToHertz (int midiNote)
{
    return 440.0 * std::exp2 ((double) (midiNote - 69) / 12.0);
}

juce::String PhibresAudioProcessor::pitchToText (float semitones)
{
    const float rounded = std::round (semitones);
    const bool whole = std::abs (semitones - rounded) < 0.005f;
    const auto number = whole ? juce::String (juce::roundToInt (rounded)) : juce::String (semitones, 2);

    return ((whole ? rounded : semitones) > 0.0f ? "+" : "") + number + " st";
}

juce::String PhibresAudioProcessor::stretchToText (float factor)
{
    return "x" + juce::String (factor, factor < 9.995f ? 2 : factor < 99.95f ? 1 : 0);
}

juce::AudioProcessorValueTreeState::ParameterLayout PhibresAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const juce::StringArray freeOrSync { "Free", "Sync" };

    // --- Capture ---
    layout.add (makeSwitch (stretchId, "Stretch", false));
    layout.add (makeChoice (lengthModeId, "Length Mode", freeOrSync, modeSync));
    layout.add (makeTimeParameter (lengthTimeId, "Length Time", minLengthMs, maxLengthMs, 1000.0f));
    layout.add (makeChoice (lengthDivisionId, "Length Division", getLengthDivisionNames(), 3));   // 1/1
    layout.add (makeChoice (lengthFeelId, "Length Feel", getFeelNames(), 0));
    layout.add (makeSwitch (grabId, "Grab", false));
    layout.add (makeSwitch (autoCaptureId, "Auto Capture", false));
    layout.add (makeChoice (autoDivisionId, "Auto Capture Every", getAutoDivisionNames(), 2));    // 4/1

    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { positionId, 1 }, "Position",
                                                             juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                                             juce::AudioParameterFloatAttributes()
                                                                 .withStringFromValueFunction ([] (float value, int) { return juce::String (value, 1) + " %"; })
                                                                 .withValueFromStringFunction (textToNumber)));

    // --- Tête de lecture ---
    layout.add (makeChoice (stretchModeId, "Stretch Mode", freeOrSync, modeFree));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { stretchFactorId, 1 }, "Stretch Factor",
                                                             makeLogRange (minStretch, maxStretch), 8.0f,
                                                             juce::AudioParameterFloatAttributes()
                                                                 .withStringFromValueFunction ([] (float value, int) { return stretchToText (value); })
                                                                 .withValueFromStringFunction ([] (const juce::String& text) { return text.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue(); })));
    layout.add (makeChoice (stretchDivisionId, "Stretch Over", getStretchDivisionNames(), 5));    // 8/1
    layout.add (makeChoice (stretchFeelId, "Stretch Feel", getFeelNames(), 0));
    layout.add (makeSwitch (freezeId, "Freeze", false));
    layout.add (makeSwitch (reverseId, "Reverse", false));

    // Pitch : en demi-tons, au centième près. En mode "Semitones" (voir pitchMode) la valeur est arrondie au demi-ton.
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { pitchId, 1 }, "Pitch",
                                                             juce::NormalisableRange<float> (-maxPitch, maxPitch, 0.01f), 0.0f,
                                                             juce::AudioParameterFloatAttributes()
                                                                 .withStringFromValueFunction ([] (float value, int) { return pitchToText (value); })
                                                                 .withValueFromStringFunction (textToNumber)));
    layout.add (makeChoice (pitchModeId, "Pitch Mode", { "Semitones", "Free" }, 0));

    // --- Grains ---
    layout.add (makeChoice (grainModeId, "Grain Mode", { "Free", "Sync", "Note" }, modeFree));
    layout.add (makeTimeParameter (grainTimeId, "Grain Time", minGrainMs, maxGrainMs, 60.0f));
    layout.add (makeChoice (grainDivisionId, "Grain Division", getGrainDivisionNames(), 5));      // 1/32
    layout.add (makeChoice (grainFeelId, "Grain Feel", getFeelNames(), 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { grainNoteId, 1 }, "Grain Note", lowestNote, highestNote, 33,
                                                           juce::AudioParameterIntAttributes()
                                                               .withStringFromValueFunction ([] (int value, int) { return noteToText (value); })
                                                               .withValueFromStringFunction ([] (const juce::String& text) { return textToNote (text); })));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { grainSizeId, 1 }, "Grain Size",
                                                             juce::NormalisableRange<float> (25.0f, 800.0f,
                                                                 [] (float start, float end, float normalised) { return start * std::pow (end / start, juce::jlimit (0.0f, 1.0f, normalised)); },
                                                                 [] (float start, float end, float value) { return juce::jlimit (0.0f, 1.0f, std::log (juce::jlimit (start, end, value) / start) / std::log (end / start)); },
                                                                 [] (float start, float end, float value) { return juce::jlimit (start, end, std::round (value)); }),
                                                             200.0f,
                                                             juce::AudioParameterFloatAttributes()
                                                                 .withStringFromValueFunction ([] (float value, int) { return juce::String (juce::roundToInt (value)) + " %"; })
                                                                 .withValueFromStringFunction (textToNumber)));
    layout.add (makePercentParameter (grainShapeId, "Grain Shape", 100.0f));
    layout.add (makePercentParameter (grainTiltId, "Grain Tilt", 0.0f, -100.0f, 100.0f));
    layout.add (makeSwitch (flipId, "Flip", false));

    // --- Dispersion ---
    layout.add (makeSwitch (scatterId, "Scatter", false));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { sprayId, 1 }, "Spray",
                                                             juce::NormalisableRange<float> (0.0f, maxSprayMs, 0.1f, 0.35f), 20.0f,
                                                             juce::AudioParameterFloatAttributes()
                                                                 .withStringFromValueFunction (timeToText)
                                                                 .withValueFromStringFunction (textToTime)));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { detuneId, 1 }, "Detune",
                                                             juce::NormalisableRange<float> (0.0f, maxDetune, 0.01f, 0.5f), 0.0f,
                                                             juce::AudioParameterFloatAttributes()
                                                                 .withStringFromValueFunction ([] (float value, int) { return juce::String (value, value < 9.995f ? 2 : 1) + " st"; })
                                                                 .withValueFromStringFunction (textToNumber)));
    layout.add (makeChoice (detuneModeId, "Detune Mode", { "Fine", "Semitones", "Octaves" }, 0));
    layout.add (makePercentParameter (flipChanceId, "Flip Chance", 0.0f));
    layout.add (makePercentParameter (widthId, "Width", 50.0f));

    // --- Sortie ---
    layout.add (makeSwitch (filterId, "Filter", false));
    layout.add (makeChoice (filterTypeId, "Filter Type", { "High Pass", "Band Pass", "Low Pass" }, 0));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { filterCutoffId, 1 }, "Filter Cutoff",
                                                             juce::NormalisableRange<float> ((float) OutputFilter::minFrequency, (float) OutputFilter::maxFrequency,
                                                                 [] (float start, float end, float normalised) { return start * std::pow (end / start, juce::jlimit (0.0f, 1.0f, normalised)); },
                                                                 [] (float start, float end, float value) { return juce::jlimit (0.0f, 1.0f, std::log (juce::jlimit (start, end, value) / start) / std::log (end / start)); },
                                                                 [] (float start, float end, float value)
                                                                 {
                                                                     const float step = value < 1000.0f ? 1.0f : 10.0f;
                                                                     return juce::jlimit (start, end, std::round (value / step) * step);
                                                                 }),
                                                             20.0f,
                                                             juce::AudioParameterFloatAttributes()
                                                                 .withStringFromValueFunction (frequencyToText)
                                                                 .withValueFromStringFunction (textToFrequency)));
    layout.add (makePercentParameter (filterResonanceId, "Filter Resonance", 20.0f));
    layout.add (makeChoice (filterSlopeId, "Filter Slope", { "12 dB", "24 dB" }, 0));
    layout.add (makeSwitch (limiterId, "Limiter", true));
    layout.add (makePercentParameter (mixId, "Mix", 100.0f));

    return layout;
}

//==============================================================================
PhibresAudioProcessor::PhibresAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Phibres", createParameterLayout())
{
    const auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };

    stretchParam         = raw (stretchId);
    lengthModeParam      = raw (lengthModeId);
    lengthTimeParam      = raw (lengthTimeId);
    lengthDivisionParam  = raw (lengthDivisionId);
    lengthFeelParam      = raw (lengthFeelId);
    grabParam            = raw (grabId);
    autoCaptureParam     = raw (autoCaptureId);
    autoDivisionParam    = raw (autoDivisionId);
    positionParam        = raw (positionId);
    stretchModeParam     = raw (stretchModeId);
    stretchFactorParam   = raw (stretchFactorId);
    stretchDivisionParam = raw (stretchDivisionId);
    stretchFeelParam     = raw (stretchFeelId);
    freezeParam          = raw (freezeId);
    reverseParam         = raw (reverseId);
    pitchParam           = raw (pitchId);
    pitchModeParam       = raw (pitchModeId);
    grainModeParam       = raw (grainModeId);
    grainTimeParam       = raw (grainTimeId);
    grainDivisionParam   = raw (grainDivisionId);
    grainFeelParam       = raw (grainFeelId);
    grainNoteParam       = raw (grainNoteId);
    grainSizeParam       = raw (grainSizeId);
    grainShapeParam      = raw (grainShapeId);
    grainTiltParam       = raw (grainTiltId);
    flipParam            = raw (flipId);
    scatterParam         = raw (scatterId);
    sprayParam           = raw (sprayId);
    detuneParam          = raw (detuneId);
    detuneModeParam      = raw (detuneModeId);
    flipChanceParam      = raw (flipChanceId);
    widthParam           = raw (widthId);
    filterParam          = raw (filterId);
    filterTypeParam      = raw (filterTypeId);
    filterCutoffParam    = raw (filterCutoffId);
    filterResonanceParam = raw (filterResonanceId);
    filterSlopeParam     = raw (filterSlopeId);
    limiterParam         = raw (limiterId);
    mixParam             = raw (mixId);
}

bool PhibresAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && layouts.getMainInputChannelSet() == out;
}

void PhibresAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Si l'hôte relance le moteur audio en plein travail (autre carte son, autre fréquence),
    // on met la capture de côté pour la retrouver juste après.
    SavedTape kept;
    const bool hadCapture = prepared && snapshotTape (kept);

    {
        const juce::ScopedLock engineGuard (engineLock);   // l'interface ne lit pas les bandes pendant qu'on les réalloue
        currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        engine.prepare (currentSampleRate, juce::jmax (1, getTotalNumOutputChannels()), maxCaptureSeconds);
    }

    // Le filtre et son limiteur démarrent dans l'état des réglages : le limiteur protège dès le premier échantillon.
    filter.setFrequency ((double) filterCutoffParam->load (std::memory_order_relaxed));
    filter.setResonance (0.01 * (double) filterResonanceParam->load (std::memory_order_relaxed));
    filter.setType (juce::roundToInt (filterTypeParam->load (std::memory_order_relaxed)));
    filter.setSteep (filterSlopeParam->load (std::memory_order_relaxed) > 0.5f);
    filter.setLimiter (limiterParam->load (std::memory_order_relaxed) > 0.5f);
    filter.setActive (filterParam->load (std::memory_order_relaxed) > 0.5f);
    filter.prepare (currentSampleRate);
    lastGrab = grabParam->load (std::memory_order_relaxed) > 0.5f;

    dryBuffer.setSize (juce::jmax (1, getTotalNumOutputChannels()), juce::jmax (4096, samplesPerBlock), false, true, false);
    mixSmoothing = (float) (1.0 - std::exp (-1.0 / (0.01 * currentSampleRate)));   // ~10 ms
    mixNow = 0.01f * mixParam->load (std::memory_order_relaxed);
    prepared = true;

    const juce::SpinLock::ScopedLockType lock (tapeLock);

    if (hadCapture && ! storedTape.isValid())
        storedTape = std::move (kept);

    if (storedTape.isValid())
        stageTape();
}

void PhibresAudioProcessor::reset()
{
    engine.reset();   // les grains s'arrêtent, la capture reste
    filter.reset();
}

void PhibresAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    const auto value = [] (const std::atomic<float>* parameter) { return parameter->load (std::memory_order_relaxed); };
    const auto isOn = [&value] (const std::atomic<float>* parameter) { return value (parameter) > 0.5f; };
    const auto choice = [&value] (const std::atomic<float>* parameter) { return juce::roundToInt (value (parameter)); };

    // --- Capture rendue par un projet rouvert : on l'installe avant tout le reste ---
    if (tapeReady.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType lock (tapeLock);

        if (lock.isLocked() && tapeReady.load (std::memory_order_relaxed))
        {
            if (isOn (stretchParam) && readyTape.getNumSamples() > 0)
                engine.loadCapture (readyTape.getArrayOfReadPointers(), readyTape.getNumChannels(), readyTape.getNumSamples(), readyPreRoll);

            storedTape.sampleRate = 0.0;   // consommée (la mémoire sera rendue hors du fil audio)
            tapeReady.store (false, std::memory_order_release);
        }
    }

    // Tempo et position de l'hôte (on garde le dernier tempo connu si l'hôte n'en fournit pas).
    double hostBeats = 0.0;
    bool hostIsMoving = false;

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition())
        {
            if (const auto bpm = position->getBpm())
                if (*bpm > 1.0)
                    lastBpm = *bpm;

            if (const auto beats = position->getPpqPosition())
            {
                hostBeats = *beats;
                hostIsMoving = position->getIsPlaying();
            }
        }
    }

    const double samplesPerBeat = 60.0 / lastBpm * currentSampleRate;
    const double samplesPerMs = 0.001 * currentSampleRate;

    // --- Dry / wet : on garde l'entrée de côté tant que le mélange n'est pas à 100 % ---
    const int numChannels = juce::jmin (numIn, numOut, buffer.getNumChannels());
    const float mixTarget = juce::jlimit (0.0f, 1.0f, 0.01f * value (mixParam));
    const bool mixing = mixTarget < 1.0f || mixNow < 1.0f;

    if (mixing)
    {
        dryBuffer.setSize (juce::jmax (dryBuffer.getNumChannels(), numChannels), juce::jmax (dryBuffer.getNumSamples(), numSamples), true, false, true);

        for (int ch = 0; ch < numChannels; ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
    }

    // --- Capture ---
    if (choice (lengthModeParam) == modeSync)
        engine.setLoopLength (getLengthDivisionInBeats (choice (lengthDivisionParam)) * getFeelFactor (choice (lengthFeelParam)) * samplesPerBeat);
    else
        engine.setLoopLength ((double) value (lengthTimeParam) * samplesPerMs);

    const double loopLength = engine.getLoopLength();   // bornée par la capacité de la bande

    engine.setAutoCapture (isOn (autoCaptureParam), getAutoDivisionInBeats (choice (autoDivisionParam)) * samplesPerBeat);
    engine.setPosition (0.01 * (double) value (positionParam));

    // --- Tête de lecture : un facteur d'étirement, ou une durée sur laquelle étaler la capture ---
    double speed = 1.0 / juce::jmax (1.0, (double) value (stretchFactorParam));

    if (choice (stretchModeParam) == modeSync)
        speed = loopLength / (getStretchDivisionInBeats (choice (stretchDivisionParam)) * getFeelFactor (choice (stretchFeelParam)) * samplesPerBeat);

    speed = juce::jlimit (0.0, GrainStretcher::maxSpeed, speed);

    const bool frozen = isOn (freezeParam);
    engine.setSpeed (speed);
    engine.setFrozen (frozen);
    engine.setBackward (isOn (reverseParam));

    loopSeconds.store ((float) (loopLength / currentSampleRate), std::memory_order_relaxed);
    passSeconds.store (frozen || speed <= 0.0 ? 0.0f : (float) (loopLength / speed / currentSampleRate), std::memory_order_relaxed);

    // --- Grains ---
    const int grainMode = choice (grainModeParam);

    if (grainMode == modeSync)
    {
        const double periodInBeats = getGrainDivisionInBeats (choice (grainDivisionParam)) * getFeelFactor (choice (grainFeelParam));
        engine.setGrainPeriod (periodInBeats * samplesPerBeat);

        // Quand le transport défile, les départs de grain tombent sur la grille de l'hôte.
        if (hostIsMoving)
            engine.syncGrainPhase (hostBeats / periodInBeats);
    }
    else if (grainMode == modeNote)
    {
        engine.setGrainPeriod (currentSampleRate / noteToHertz (choice (grainNoteParam)));
    }
    else
    {
        engine.setGrainPeriod ((double) value (grainTimeParam) * samplesPerMs);
    }

    engine.setGrainSize (0.01 * (double) value (grainSizeParam));
    engine.setShape (0.01 * (double) value (grainShapeParam));
    engine.setTilt (0.01 * (double) value (grainTiltParam));
    engine.setFlip (isOn (flipParam));

    // Pitch : en demi-tons entiers, ou libre
    const double semitones = (double) value (pitchParam);
    engine.setPitch (isOn (pitchModeParam) ? semitones : std::round (semitones));

    engine.setScatter (isOn (scatterParam), (double) value (sprayParam) * samplesPerMs, (double) value (detuneParam),
                       choice (detuneModeParam), 0.01 * (double) value (flipChanceParam), 0.01 * (double) value (widthParam));

    // --- Recapture : bouton GRAB (front montant) ou note MIDI ---
    const bool grab = isOn (grabParam);

    if (grab && ! lastGrab)
        engine.trigger();

    lastGrab = grab;
    engine.setActive (isOn (stretchParam));

    // --- Filtre ---
    filter.setFrequency ((double) value (filterCutoffParam));
    filter.setResonance (0.01 * (double) value (filterResonanceParam));
    filter.setType (choice (filterTypeParam));
    filter.setSteep (isOn (filterSlopeParam));
    filter.setLimiter (isOn (limiterParam));
    filter.setActive (isOn (filterParam));

    // --- Grains, découpés aux notes MIDI pour que la recapture tombe à l'échantillon près ---
    const auto channels = buffer.getArrayOfWritePointers();
    int done = 0;

    const auto runEngine = [this, channels, numChannels, &done] (int until)
    {
        if (until <= done)
            return;

        float* offset[GrainStretcher::maxChannels] = {};

        for (int ch = 0; ch < juce::jmin (numChannels, (int) GrainStretcher::maxChannels); ++ch)
            offset[ch] = channels[ch] + done;

        engine.process (offset, numChannels, until - done);
        done = until;
    };

    for (const auto metadata : midi)
    {
        if (metadata.getMessage().isNoteOn())
        {
            runEngine (juce::jlimit (0, numSamples, metadata.samplePosition));
            engine.trigger();
        }
    }

    runEngine (numSamples);

    filter.process (channels, numChannels, numSamples);

    // --- Mélange avec l'entrée ---
    if (mixing)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            const float delta = mixTarget - mixNow;
            mixNow = std::abs (delta) < 1.0e-4f ? mixTarget : mixNow + delta * mixSmoothing;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float dry = dryBuffer.getSample (ch, n);
                channels[ch][n] = dry + mixNow * (channels[ch][n] - dry);
            }
        }
    }
}

void PhibresAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // En bypass on repart d'un état propre : pas de vieille capture rejouée à la réactivation.
    engine.clear();
    filter.reset();
    AudioProcessor::processBlockBypassed (buffer, midi);
}

//==============================================================================
juce::AudioProcessorEditor* PhibresAudioProcessor::createEditor()
{
    return new PhibresAudioProcessorEditor (*this);
}

bool PhibresAudioProcessor::snapshotTape (SavedTape& tape) const
{
    if (! prepared || stretchParam->load (std::memory_order_relaxed) < 0.5f)
        return false;

    const juce::ScopedLock engineGuard (engineLock);

    // Le fil audio peut redémarrer une bande pendant la copie : on revérifie après, et on recommence au besoin.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        GrainStretcher::View view;
        const auto sequence = engine.getLayoutSeq();

        if (engine.getView (view))
        {
            if (! view.running)
                return false;

            const int preRoll = juce::roundToInt (savedPreRollSeconds * currentSampleRate);
            const int postRoll = juce::roundToInt (savedPostRollSeconds * currentSampleRate);
            const int from = juce::jmax (view.begin, view.origin - preRoll);
            const int to = juce::jmin (view.end, view.origin + (int) std::ceil (view.loopLength) + postRoll);
            const int numChannels = engine.getNumChannels();

            if (to - from < 64 || view.origin < from || view.origin >= to)
                return false;

            tape.audio.setSize (numChannels, to - from, false, false, false);

            for (int ch = 0; ch < numChannels; ++ch)
                tape.audio.copyFrom (ch, 0, engine.getTapeData (view.tape, ch) + from, to - from);

            if (engine.getLayoutSeq() == sequence)
            {
                tape.sampleRate = currentSampleRate;
                tape.preRoll = view.origin - from;
                return true;
            }
        }

        juce::Thread::sleep (2);
    }

    return false;
}

void PhibresAudioProcessor::writeTape (const SavedTape& tape, juce::OutputStream& out)
{
    const int numChannels = tape.audio.getNumChannels(), numSamples = tape.audio.getNumSamples();
    const float scale = juce::jmax (1.0f, tape.audio.getMagnitude (0, numSamples));   // un signal au-dessus de 0 dBFS n'est pas écrêté

    out.writeInt (tapeMagic);
    out.writeInt (tapeVersion);
    out.writeDouble (tape.sampleRate);
    out.writeInt (numChannels);
    out.writeInt (tape.preRoll);
    out.writeInt (numSamples);
    out.writeFloat (scale);

    // Échantillons entrelacés, sur 24 bits.
    juce::HeapBlock<juce::uint8> bytes ((size_t) numChannels * (size_t) numSamples * 3);
    auto* destination = bytes.getData();

    for (int n = 0; n < numSamples; ++n)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float x = tape.audio.getSample (ch, n) / scale;
            const int v = juce::roundToInt (juce::jlimit (-1.0f, 1.0f, std::isfinite (x) ? x : 0.0f) * 8388607.0f);
            *destination++ = (juce::uint8) (v & 0xff);
            *destination++ = (juce::uint8) ((v >> 8) & 0xff);
            *destination++ = (juce::uint8) ((v >> 16) & 0xff);
        }
    }

    out.write (bytes.getData(), (size_t) numChannels * (size_t) numSamples * 3);
}

bool PhibresAudioProcessor::readTape (juce::InputStream& in, SavedTape& tape)
{
    if (in.readInt() != tapeMagic || in.readInt() != tapeVersion)
        return false;

    const double sampleRate = in.readDouble();
    const int numChannels = in.readInt();
    const int preRoll = in.readInt();
    const int numSamples = in.readInt();
    const float scale = in.readFloat();

    if (! (sampleRate >= 1000.0 && sampleRate <= 1.0e6) || numChannels < 1 || numChannels > GrainStretcher::maxChannels
        || numSamples < 1 || numSamples > 64 * 1024 * 1024 || preRoll < 0 || preRoll >= numSamples
        || ! (scale >= 1.0f && scale < 1.0e6f)
        || in.getNumBytesRemaining() < (juce::int64) numChannels * numSamples * 3)
        return false;

    juce::HeapBlock<juce::uint8> bytes ((size_t) numChannels * (size_t) numSamples * 3);

    if (in.read (bytes.getData(), numChannels * numSamples * 3) != numChannels * numSamples * 3)
        return false;

    tape.audio.setSize (numChannels, numSamples, false, false, false);
    const auto* source = bytes.getData();
    const float gain = scale / 8388607.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            int v = (int) source[0] | ((int) source[1] << 8) | ((int) source[2] << 16);

            if (v & 0x800000)   // le bit de poids fort porte le signe
                v -= 0x1000000;

            tape.audio.setSample (ch, n, (float) v * gain);
            source += 3;
        }
    }

    tape.sampleRate = sampleRate;
    tape.preRoll = preRoll;
    return true;
}

void PhibresAudioProcessor::stageTape()
{
    tapeReady.store (false, std::memory_order_release);

    if (! prepared || ! storedTape.isValid())
        return;

    const double ratio = currentSampleRate / storedTape.sampleRate;
    const int sourceLength = storedTape.audio.getNumSamples();
    const int numChannels = storedTape.audio.getNumChannels();
    const int length = juce::jmin (engine.getTapeCapacity(), juce::roundToInt ((double) sourceLength * ratio));

    if (length < 64)
        return;

    readyTape.setSize (numChannels, length, false, false, false);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* source = storedTape.audio.getReadPointer (ch);
        float* destination = readyTape.getWritePointer (ch);

        if (std::abs (ratio - 1.0) < 1.0e-9)
        {
            std::copy (source, source + length, destination);
            continue;
        }

        // Autre fréquence d'échantillonnage qu'à la sauvegarde : on rééchantillonne (interpolation cubique).
        const auto at = [source, sourceLength] (int index) { return source[juce::jlimit (0, sourceLength - 1, index)]; };

        for (int n = 0; n < length; ++n)
        {
            const double position = (double) n / ratio;
            const int index = (int) position;
            destination[n] = hermite (at (index - 1), at (index), at (index + 1), at (index + 2), (float) (position - index));
        }
    }

    readyPreRoll = juce::jlimit (0, length - 1, juce::roundToInt ((double) storedTape.preRoll * ratio));
    tapeReady.store (true, std::memory_order_release);
}

void PhibresAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryBlock xmlBlock;

    if (const auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, xmlBlock);

    // La capture en cours ; à défaut, celle qu'un projet vient de rendre et qui n'a pas encore été jouée.
    juce::MemoryOutputStream tapeStream;
    SavedTape tape;

    if (snapshotTape (tape))
    {
        writeTape (tape, tapeStream);
    }
    else
    {
        const juce::SpinLock::ScopedLockType lock (tapeLock);

        if (storedTape.isValid() && stretchParam->load (std::memory_order_relaxed) > 0.5f)
            writeTape (storedTape, tapeStream);
    }

    juce::MemoryOutputStream out (destData, false);
    out.writeInt (containerMagic);
    out.writeInt ((int) xmlBlock.getSize());
    out.write (xmlBlock.getData(), xmlBlock.getSize());
    out.writeInt ((int) tapeStream.getDataSize());
    out.write (tapeStream.getData(), tapeStream.getDataSize());
}

void PhibresAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml;
    SavedTape tape;
    bool hasTape = false;

    juce::MemoryInputStream in (data, (size_t) juce::jmax (0, sizeInBytes), false);

    if (sizeInBytes >= 12 && in.readInt() == containerMagic)
    {
        const int xmlSize = in.readInt();

        if (xmlSize < 0 || xmlSize > sizeInBytes - 12)
            return;

        xml = getXmlFromBinary (juce::addBytesToPointer (data, 8), xmlSize);
        in.setPosition (8 + xmlSize);

        const int tapeSize = in.readInt();

        if (tapeSize > 0 && tapeSize <= in.getNumBytesRemaining())
            hasTape = readTape (in, tape);
    }
    else
    {
        xml = getXmlFromBinary (data, sizeInBytes);   // des paramètres seuls, sans capture
    }

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    const juce::SpinLock::ScopedLockType lock (tapeLock);
    tapeReady.store (false, std::memory_order_release);
    storedTape = hasTape ? std::move (tape) : SavedTape();

    if (prepared)
        stageTape();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PhibresAudioProcessor();
}
