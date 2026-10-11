#pragma once

#include "PluginProcessor.h"

/**
    Design clair, celui de Phragments : plaques à pans coupés, gris clair et
    blanc, accent en dégradé bleu -> cyan. Le relief vient d'ombres portées (ce
    qui dépasse) et d'ombres intérieures (ce qui est en creux). Police Chakra
    Petch embarquée.
*/
class PhibresLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    PhibresLookAndFeel();

    // Palette
    static juce::Colour ground()    { return juce::Colour (0xffd9dfe7); }
    static juce::Colour plate()     { return juce::Colour (0xfff5f7fa); }
    static juce::Colour bay()       { return juce::Colour (0xffe6eaf0); }
    static juce::Colour line()      { return juce::Colour (0xffb4becb); }
    static juce::Colour seam()      { return juce::Colour (0xffc5ced9); }
    static juce::Colour track()     { return juce::Colour (0xffcdd5df); }
    static juce::Colour ink()       { return juce::Colour (0xff101622); }
    static juce::Colour dimInk()    { return juce::Colour (0xff5b6676); }
    static juce::Colour blue()      { return juce::Colour (0xff1785ff); }
    static juce::Colour cyan()      { return juce::Colour (0xff1de4f5); }
    static juce::Colour standby()   { return juce::Colour (0xff93a3bf); }
    static juce::Colour unlit()     { return juce::Colour (0xffc3ccd8); }
    static juce::Colour shadow()    { return juce::Colour (0xff1b2b4b); }

    /** Dégradé d'accent, du bleu au cyan. holdBlue garde le bleu sur les deux premiers tiers (texte blanc lisible). */
    static juce::ColourGradient accentGradient (juce::Point<float> from, juce::Point<float> to, bool holdBlue = false);
    static juce::Colour accentAt (float proportion);

    static juce::Path chamferedRect (juce::Rectangle<float> area, float cut);

    /** Trait en dégradé avec un halo lumineux autour. */
    static void strokeGlowing (juce::Graphics&, const juce::Path&, float thickness,
                               const juce::ColourGradient&, bool roundedEnds = true);

    /** Ombre portée sous une forme : elle la décolle du fond. Coûteuse : à réserver au fond, dessiné une seule fois. */
    static void dropShadow (juce::Graphics&, const juce::Path&, float radius, float offsetY, float alpha);

    /** Ombre intérieure en haut d'une forme : elle la creuse. Même remarque. */
    static void innerShadow (juce::Graphics&, const juce::Path&, float radius, float offsetY, float alpha);

    /** Remplissage en dégradé vertical. */
    static void fillVertical (juce::Graphics&, const juce::Path&, juce::Rectangle<float> area, juce::Colour top, juce::Colour bottom);

    /** Écran blanc en creux (afficheurs). */
    static void drawScreen (juce::Graphics&, juce::Rectangle<float> area, float cut);

    juce::Font font (float pointHeight, bool bold, float tracking = 0.0f) const;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getLabelFont (juce::Label&) override;

    /** Rayon du corps d'un grand / petit potard pour une zone donnée (le fond dessine son ombre). */
    static float mainKnobBodyRadius (juce::Rectangle<float> bounds)   { return 58.0f * juce::jmin (bounds.getWidth(), bounds.getHeight()) / 190.0f; }
    static float miniKnobBodyRadius (juce::Rectangle<float> bounds)   { return juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 8.5f; }

private:
    void drawMainKnob (juce::Graphics&, juce::Rectangle<float> bounds, float sliderPos,
                       float startAngle, float endAngle, juce::Slider&);
    void drawMiniKnob (juce::Graphics&, juce::Rectangle<float> bounds, float sliderPos,
                       float startAngle, float endAngle);

    juce::Typeface::Ptr semiBoldFace, boldFace;
};

//==============================================================================
/**
    La bande : forme d'onde de la boucle capturée, partie encore à enregistrer
    en pointillé, et tête de lecture. Se rafraîchit toute seule.
*/
class TapeDisplay final : public juce::Component,
                          private juce::Timer
{
public:
    explicit TapeDisplay (PhibresAudioProcessor&);

    void paint (juce::Graphics&) override;

    /** Relit la bande tout de suite (les tests s'en servent ; sinon le minuteur s'en charge). */
    void refresh();

    bool isShowingCapture() const noexcept      { return running; }
    float getShownPlayhead() const noexcept     { return playhead; }
    float getShownRecorded() const noexcept     { return recorded; }

private:
    static constexpr int numBins = 176;

    void timerCallback() override               { refresh(); }
    juce::Rectangle<float> plotArea() const     { return getLocalBounds().toFloat().reduced (7.0f, 6.0f); }

    PhibresAudioProcessor& audioProcessor;

    std::array<float, (size_t) numBins> low {}, high {};
    bool running = false;
    float playhead = 0.0f, recorded = 0.0f;   // de 0 à 1 dans la boucle

    // Ce qui a servi au dernier calcul de la forme d'onde
    unsigned int shownSequence = 0xffffffffu, shownCaptures = 0xffffffffu;
    int shownOrigin = -1, shownEnd = -1;
    double shownLoop = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeDisplay)
};

//==============================================================================
/**
    Cinq modules côte à côte : CAPTURE, STRETCH, GRAIN, SCATTER, FILTER.
    Chacun a son grand potard, son afficheur, ses interrupteurs, son grand
    bouton et un panneau de réglages en dessous.
*/
class PhibresAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit PhibresAudioProcessorEditor (PhibresAudioProcessor&);
    ~PhibresAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using APVTS = juce::AudioProcessorValueTreeState;

    /** Interrupteur à plusieurs positions relié à un paramètre à choix. */
    struct ChoiceSwitch
    {
        juce::OwnedArray<juce::TextButton> buttons;
        std::unique_ptr<juce::ParameterAttachment> attachment;
    };

    /** Variante rythmique d'un module en SYNC : T (triolet) et D (pointé), tous deux éteints = normal. */
    struct FeelSwitch
    {
        juce::TextButton triplet { "T" }, dotted { "D" };
        std::unique_ptr<juce::ParameterAttachment> attachment;
        int value = 0;
    };

    void configureMainKnob (juce::Slider&, const char* parameterId, int column);
    void configureMiniKnob (juce::Slider&, const char* parameterId, const char* name, int column, int slot, int numSlots);
    void configureValueLabel (juce::Label&, int column);
    void configureBigToggle (juce::TextButton&, const char* parameterId, int column, std::function<void (bool)> onChange);
    void configureSmallToggle (juce::TextButton&, const char* parameterId, juce::Rectangle<int> area);
    void configureSwitch (ChoiceSwitch&, const char* parameterId, const juce::StringArray& texts,
                          juce::Rectangle<int> area, bool tiny, std::function<void (int)> onChange);
    void configureFeel (FeelSwitch&, const char* parameterId, int column);

    /** Le potard affiché dans un module (selon son mode : ms, divisions ou notes). */
    juce::Slider& activeKnob (int column);
    bool isSynced (int column) const;
    void setStandby (int column, bool standby);
    void modesChanged();
    void updateValueLabels();

    void timerCallback() override;

    void paintBackground (juce::Graphics&);
    void paintLights (juce::Graphics&);
    void paintModules (juce::Graphics&);

    PhibresAudioProcessor& audioProcessor;
    PhibresLookAndFeel lookAndFeel;

    // Fond (plaques, ombres, textes fixes) : dessiné une fois, à la résolution de l'écran.
    juce::Image background;
    float backgroundScale = 0.0f;

    bool stretchOn = false, freezeOn = false, flipOn = false, scatterOn = false, filterOn = false, reverseOn = false;
    int lengthMode = 1, stretchMode = 0, grainMode = 0, detuneMode = 0, filterType = 0;
    bool pitchInSemitones = true, steepFilter = false, grabDown = false;
    float shownLoopSeconds = -1.0f, shownPassSeconds = -1.0f;

    // Les grands potards sont faits de sliders superposés, un par mode.
    juce::Slider lengthTimeKnob, lengthDivisionKnob, stretchFactorKnob, stretchDivisionKnob;
    juce::Slider grainTimeKnob, grainDivisionKnob, grainNoteKnob, sprayKnob, cutoffKnob;
    juce::Slider positionKnob, everyKnob, pitchKnob, sizeKnob, shapeKnob, tiltKnob, detuneKnob, flipChanceKnob, widthKnob, resonanceKnob, mixKnob;
    juce::Label valueLabels[5];
    juce::TextButton stretchButton { "STRETCH" }, freezeButton { "FREEZE" }, flipButton { "FLIP" }, scatterButton { "SCATTER" }, filterButton { "FILTER" };
    juce::TextButton autoButton { "AUTO" }, grabButton { "GRAB" }, reverseButton { "REVERSE" }, limiterButton { "LIMITER" };
    ChoiceSwitch lengthModeSwitch, stretchModeSwitch, grainModeSwitch, detuneModeSwitch, filterTypeSwitch, filterSlopeSwitch, pitchModeSwitch;
    FeelSwitch lengthFeel, stretchFeel, grainFeel;
    TapeDisplay tapeDisplay;

    // Déclarés après les contrôles : détruits avant eux.
    std::vector<std::unique_ptr<APVTS::SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<APVTS::ButtonAttachment>> buttonAttachments;
    std::vector<std::unique_ptr<juce::ParameterAttachment>> stateWatchers;
    std::unique_ptr<juce::ParameterAttachment> pitchAttachment, grabAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhibresAudioProcessorEditor)
};
