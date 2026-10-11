#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "GrainStretcher.h"
#include "OutputFilter.h"

class PhibresAudioProcessor final : public juce::AudioProcessor
{
public:
    //==========================================================================
    // Identifiants des paramètres (ne pas les renommer : les projets DAW sauvegardés en dépendent)

    // Capture
    static constexpr const char* stretchId         = "stretch";
    static constexpr const char* lengthModeId      = "lengthMode";
    static constexpr const char* lengthTimeId      = "lengthTime";
    static constexpr const char* lengthDivisionId  = "lengthDivision";
    static constexpr const char* lengthFeelId      = "lengthFeel";
    static constexpr const char* grabId            = "grab";
    static constexpr const char* autoCaptureId     = "autoCapture";
    static constexpr const char* autoDivisionId    = "autoDivision";
    static constexpr const char* positionId        = "position";

    // Tête de lecture
    static constexpr const char* stretchModeId     = "stretchMode";
    static constexpr const char* stretchFactorId   = "stretchFactor";
    static constexpr const char* stretchDivisionId = "stretchDivision";
    static constexpr const char* stretchFeelId     = "stretchFeel";
    static constexpr const char* freezeId          = "freeze";
    static constexpr const char* reverseId         = "reverse";
    static constexpr const char* pitchId           = "pitch";
    static constexpr const char* pitchModeId       = "pitchMode";

    // Grains
    static constexpr const char* grainModeId       = "grainMode";
    static constexpr const char* grainTimeId       = "grainTime";
    static constexpr const char* grainDivisionId   = "grainDivision";
    static constexpr const char* grainFeelId       = "grainFeel";
    static constexpr const char* grainNoteId       = "grainNote";
    static constexpr const char* grainSizeId       = "grainSize";
    static constexpr const char* grainShapeId      = "grainShape";
    static constexpr const char* grainTiltId       = "grainTilt";
    static constexpr const char* flipId            = "flip";

    // Dispersion
    static constexpr const char* scatterId         = "scatter";
    static constexpr const char* sprayId           = "spray";
    static constexpr const char* detuneId          = "detune";
    static constexpr const char* detuneModeId      = "detuneMode";
    static constexpr const char* flipChanceId      = "flipChance";
    static constexpr const char* widthId           = "width";

    // Sortie
    static constexpr const char* filterId          = "filter";
    static constexpr const char* filterTypeId      = "filterType";
    static constexpr const char* filterCutoffId    = "filterCutoff";
    static constexpr const char* filterResonanceId = "filterResonance";
    static constexpr const char* filterSlopeId     = "filterSlope";
    static constexpr const char* limiterId         = "limiter";
    static constexpr const char* mixId             = "mix";

    // Plages du mode free
    static constexpr float minLengthMs = 20.0f;
    static constexpr float maxLengthMs = 30000.0f;
    static constexpr float minGrainMs = 1.0f;
    static constexpr float maxGrainMs = 1000.0f;
    static constexpr float minStretch = 1.0f;
    static constexpr float maxStretch = 1000.0f;
    static constexpr float maxSprayMs = 1000.0f;
    static constexpr float maxDetune = 24.0f;
    static constexpr float maxPitch = 24.0f;
    static constexpr int lowestNote = 12, highestNote = 84;   // de 16 Hz à 1047 Hz

    static constexpr double maxCaptureSeconds = 30.0;

    enum Mode { modeFree = 0, modeSync = 1, modeNote = 2 };

    /** Variante d'une division en SYNC : normale, triolet (x 2/3) ou pointée (x 3/2). */
    static const juce::StringArray& getFeelNames();
    static double getFeelFactor (int index);

    /** Longueur de la capture en SYNC, de la plus longue à la plus courte (8/1 = 8 mesures). */
    static const juce::StringArray& getLengthDivisionNames();
    static double getLengthDivisionInBeats (int index);

    /** Intervalle de recapture automatique, du plus long au plus court. */
    static const juce::StringArray& getAutoDivisionNames();
    static double getAutoDivisionInBeats (int index);

    /** Durée sur laquelle la capture est étalée en SYNC, de la plus courte à la plus longue. */
    static const juce::StringArray& getStretchDivisionNames();
    static double getStretchDivisionInBeats (int index);

    /** Cadence des grains en SYNC, de la plus lente à la plus rapide. */
    static const juce::StringArray& getGrainDivisionNames();
    static double getGrainDivisionInBeats (int index);

    /** Nom d'une note à la façon d'Ableton Live (do central = C3 = note MIDI 60) et sa fréquence. */
    static juce::String noteToText (int midiNote);
    static int textToNote (const juce::String& text);
    static double noteToHertz (int midiNote);

    /** Texte d'une transposition : "+3 st", ou "+3.25 st" quand elle ne tombe pas sur un demi-ton. */
    static juce::String pitchToText (float semitones);

    /** "x8.00", "x25.0", "x250" */
    static juce::String stretchToText (float factor);

    //==========================================================================
    PhibresAudioProcessor();
    ~PhibresAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return "Phibres"; }
    bool acceptsMidi() const override                       { return true; }    // une note = une recapture
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    //==========================================================================
    // Pour l'interface (lecture seule, depuis le fil des messages)
    const GrainStretcher& getEngine() const noexcept        { return engine; }

    /** À tenir pendant qu'on lit les bandes hors du fil audio : prepareToPlay les réalloue. */
    const juce::CriticalSection& getEngineLock() const noexcept { return engineLock; }
    double getCurrentSampleRate() const noexcept            { return currentSampleRate; }

    /** Durée de la boucle capturée, en secondes. */
    float getLoopSeconds() const noexcept                   { return loopSeconds.load (std::memory_order_relaxed); }

    /** Durée d'un passage de la tête sur la boucle, en secondes (0 = tête gelée). */
    float getPassSeconds() const noexcept                   { return passSeconds.load (std::memory_order_relaxed); }

    /** Une capture mise de côté (sauvegarde du projet, changement de fréquence d'échantillonnage). */
    struct SavedTape
    {
        double sampleRate = 0.0;
        int preRoll = 0;                      // nombre d'échantillons avant la position 0
        juce::AudioBuffer<float> audio;

        bool isValid() const noexcept         { return sampleRate > 0.0 && audio.getNumChannels() > 0 && audio.getNumSamples() > 0; }
    };

    /** Copie la capture en cours (false s'il n'y en a pas). À appeler hors du fil audio. */
    bool snapshotTape (SavedTape&) const;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    static void writeTape (const SavedTape&, juce::OutputStream&);
    static bool readTape (juce::InputStream&, SavedTape&);

    /** Met la capture en attente à la fréquence d'échantillonnage courante, prête pour le fil audio. Verrou tenu. */
    void stageTape();

    // Chaîne : grains, filtre (et limiteur), puis mélange dry / wet.
    juce::CriticalSection engineLock;
    GrainStretcher engine;
    OutputFilter filter;

    std::atomic<float>* stretchParam = nullptr;
    std::atomic<float>* lengthModeParam = nullptr;
    std::atomic<float>* lengthTimeParam = nullptr;
    std::atomic<float>* lengthDivisionParam = nullptr;
    std::atomic<float>* lengthFeelParam = nullptr;
    std::atomic<float>* grabParam = nullptr;
    std::atomic<float>* autoCaptureParam = nullptr;
    std::atomic<float>* autoDivisionParam = nullptr;
    std::atomic<float>* positionParam = nullptr;
    std::atomic<float>* stretchModeParam = nullptr;
    std::atomic<float>* stretchFactorParam = nullptr;
    std::atomic<float>* stretchDivisionParam = nullptr;
    std::atomic<float>* stretchFeelParam = nullptr;
    std::atomic<float>* freezeParam = nullptr;
    std::atomic<float>* reverseParam = nullptr;
    std::atomic<float>* pitchParam = nullptr;
    std::atomic<float>* pitchModeParam = nullptr;
    std::atomic<float>* grainModeParam = nullptr;
    std::atomic<float>* grainTimeParam = nullptr;
    std::atomic<float>* grainDivisionParam = nullptr;
    std::atomic<float>* grainFeelParam = nullptr;
    std::atomic<float>* grainNoteParam = nullptr;
    std::atomic<float>* grainSizeParam = nullptr;
    std::atomic<float>* grainShapeParam = nullptr;
    std::atomic<float>* grainTiltParam = nullptr;
    std::atomic<float>* flipParam = nullptr;
    std::atomic<float>* scatterParam = nullptr;
    std::atomic<float>* sprayParam = nullptr;
    std::atomic<float>* detuneParam = nullptr;
    std::atomic<float>* detuneModeParam = nullptr;
    std::atomic<float>* flipChanceParam = nullptr;
    std::atomic<float>* widthParam = nullptr;
    std::atomic<float>* filterParam = nullptr;
    std::atomic<float>* filterTypeParam = nullptr;
    std::atomic<float>* filterCutoffParam = nullptr;
    std::atomic<float>* filterResonanceParam = nullptr;
    std::atomic<float>* filterSlopeParam = nullptr;
    std::atomic<float>* limiterParam = nullptr;
    std::atomic<float>* mixParam = nullptr;

    double currentSampleRate = 44100.0;
    double lastBpm = 120.0;
    bool lastGrab = false;
    bool prepared = false;

    std::atomic<float> loopSeconds { 1.0f }, passSeconds { 8.0f };

    // Dry / wet : copie de l'entrée et mélange lissé
    juce::AudioBuffer<float> dryBuffer;
    float mixNow = 1.0f, mixSmoothing = 1.0f;

    // Capture rendue par un projet rouvert : décodée par setStateInformation, installée par le fil audio.
    mutable juce::SpinLock tapeLock;
    SavedTape storedTape;                    // telle que sauvegardée (à sa fréquence d'origine)
    juce::AudioBuffer<float> readyTape;      // à la fréquence courante
    int readyPreRoll = 0;
    std::atomic<bool> tapeReady { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhibresAudioProcessor)
};
