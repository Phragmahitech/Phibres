#include "PluginEditor.h"
#include "BinaryData.h"

#ifndef PHIBRES_VERSION
 #define PHIBRES_VERSION "dev"
#endif

namespace
{
    using LF = PhibresLookAndFeel;
    using Pt = juce::Point<float>;

    // Façade : cinq modules côte à côte (CAPTURE, STRETCH, GRAIN, SCATTER, FILTER), tous bâtis sur la même grille.
    constexpr int numColumns = 5, columnWidth = 222, columnGap = 14, firstColumnX = 26;
    constexpr int editorWidth = 2 * firstColumnX + numColumns * columnWidth + (numColumns - 1) * columnGap, editorHeight = 642;
    constexpr int panelInset = 12, panelInnerWidth = columnWidth - 2 * panelInset;
    constexpr int footerY = 610;

    constexpr int columnX (int column)                  { return firstColumnX + column * (columnWidth + columnGap); }

    juce::Rectangle<int> bayArea (int column)           { return { columnX (column), 84, columnWidth, 268 }; }
    juce::Rectangle<int> knobArea (int column)          { return { columnX (column) + 31, 100, 160, 160 }; }
    juce::Rectangle<int> readoutArea (int column)       { return { columnX (column) + 16, 276, columnWidth - 32, 34 }; }
    juce::Rectangle<int> switchArea (int column, int offset, int width)   { return { columnX (column) + offset, 318, width, 24 }; }
    juce::Rectangle<int> buttonArea (int column)        { return { columnX (column), 364, columnWidth, 52 }; }
    juce::Rectangle<int> panelArea (int column)         { return { columnX (column), 430, columnWidth, 166 }; }
    juce::Rectangle<int> displayArea (int column)       { return { columnX (column) + panelInset, 458, panelInnerWidth, 44 }; }

    // Interrupteurs d'une baie : le mode à gauche, la variante (T / D, ou la pente du filtre) à droite.
    constexpr int modeOffset = 16, modeWidth = 112, feelOffset = 132, feelWidth = 37;

    /** Petit potard numéro "slot" parmi "numSlots" répartis sur la largeur du panneau. */
    juce::Rectangle<int> miniKnobArea (int column, int slot, int numSlots)
    {
        const int centre = columnX (column) + panelInset + (panelInnerWidth * (2 * slot + 1)) / (2 * numSlots);
        return { centre - 20, 514, 40, 40 };
    }

    /** Dernier tiers d'un panneau, à droite de deux petits potards : une ou deux touches empilées. */
    juce::Rectangle<int> panelKeyArea (int column, int row, int numRows)
    {
        const int x = columnX (column) + panelInset + (2 * panelInnerWidth) / 3 + 2;
        const int width = columnX (column) + columnWidth - panelInset - x;

        if (numRows == 1)
            return { x, 522, width, 24 };

        return { x, 512 + row * 24, width, 20 };
    }

    juce::Path polyline (std::initializer_list<Pt> points, bool closed = false)
    {
        juce::Path path;
        bool first = true;

        for (const auto point : points)
        {
            if (first)
                path.startNewSubPath (point);
            else
                path.lineTo (point);

            first = false;
        }

        if (closed)
            path.closeSubPath();

        return path;
    }

    juce::Path circle (Pt centre, float radius)
    {
        juce::Path path;
        path.addEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));
        return path;
    }

    /** "4.00 s" -> "4 s", "10.0 ms" -> "10 ms", "x1.00" -> "x1" : pour les repères de début et de fin des potards. */
    juce::String compactValue (const juce::String& text)
    {
        auto compact = text.replace (".00 ", " ").replace (".0 ", " ");

        if (compact.endsWith (".00"))
            compact = compact.dropLastCharacters (3);
        else if (compact.endsWith (".0"))
            compact = compact.dropLastCharacters (2);

        return compact;
    }

    float textWidth (const juce::Font& font, const juce::String& text)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, text, 0.0f, 0.0f);
        return glyphs.getBoundingBox (0, -1, true).getWidth();
    }

    /** Halo lumineux autour d'une forme allumée. */
    void drawHalo (juce::Graphics& g, const juce::Path& outline, const juce::ColourGradient& gradient)
    {
        struct Layer { float width, alpha; };

        for (const auto layer : { Layer { 16.0f, 0.06f }, Layer { 10.0f, 0.09f }, Layer { 5.0f, 0.13f } })
        {
            g.setGradientFill (gradient);
            g.setOpacity (layer.alpha);
            g.strokePath (outline, juce::PathStrokeType (layer.width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setOpacity (1.0f);
    }

    /** Trace une courbe en dégradé d'accent, remplie en dessous jusqu'au bas de la zone. */
    void plotCurve (juce::Graphics& g, juce::Rectangle<float> plot, const juce::Path& curve, float thickness = 2.0f)
    {
        auto area = curve;
        area.lineTo (plot.getBottomRight());
        area.lineTo (plot.getBottomLeft());
        area.closeSubPath();

        const auto gradient = LF::accentGradient (plot.getBottomLeft(), plot.getBottomRight());
        g.setGradientFill (gradient);
        g.setOpacity (0.16f);
        g.fillPath (area);
        g.setGradientFill (gradient);
        g.setOpacity (1.0f);
        g.strokePath (curve, juce::PathStrokeType (thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    }

    /** "300 ms", "9.60 s", "4.2 min", "1.5 h" */
    juce::String durationText (double seconds)
    {
        if (seconds < 0.9995)
            return juce::String (juce::roundToInt (seconds * 1000.0)) + " ms";

        if (seconds < 99.995)
            return juce::String (seconds, 2) + " s";

        if (seconds < 3600.0)
            return juce::String (seconds / 60.0, 1) + " min";

        return juce::String (seconds / 3600.0, 1) + " h";
    }

    /** Fenêtre d'un grain telle que le moteur la calcule, pour un avancement de 0 à 1 (sans le minimum de 1 ms). */
    double windowShape (double phase, double shape, double tilt)
    {
        if (phase <= 0.0 || phase >= 1.0)
            return 0.0;

        double attack = 0.0, release = 0.0;
        GrainStretcher::windowFractions (shape, tilt, attack, release);
        attack = juce::jmax (0.012, attack);
        release = juce::jmax (0.012, release);

        const auto cosine = [] (double x) { return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * juce::jlimit (0.0, 1.0, x)); };

        if (phase < attack)
            return cosine (phase / attack);

        return 1.0 - phase < release ? cosine ((1.0 - phase) / release) : 1.0;
    }
}

//==============================================================================
PhibresLookAndFeel::PhibresLookAndFeel()
{
    semiBoldFace = juce::Typeface::createSystemTypefaceFor (BinaryData::ChakraPetchSemiBold_ttf,
                                                            (size_t) BinaryData::ChakraPetchSemiBold_ttfSize);
    boldFace = juce::Typeface::createSystemTypefaceFor (BinaryData::ChakraPetchBold_ttf,
                                                        (size_t) BinaryData::ChakraPetchBold_ttfSize);

    setColour (juce::ResizableWindow::backgroundColourId, ground());

    setColour (juce::Label::textColourId, ink());
    setColour (juce::Label::textWhenEditingColourId, ink());
    setColour (juce::Label::backgroundWhenEditingColourId, juce::Colours::white);
    setColour (juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::textColourId, ink());
    setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
    setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::highlightColourId, cyan().withAlpha (0.45f));
    setColour (juce::TextEditor::highlightedTextColourId, ink());
    setColour (juce::CaretComponent::caretColourId, blue());
}

juce::ColourGradient PhibresLookAndFeel::accentGradient (juce::Point<float> from, juce::Point<float> to, bool holdBlue)
{
    juce::ColourGradient gradient (blue(), from, cyan(), to, false);

    if (holdBlue)
        gradient.addColour (0.62, juce::Colour (0xff1a8dff));

    return gradient;
}

juce::Colour PhibresLookAndFeel::accentAt (float proportion)
{
    return blue().interpolatedWith (cyan(), juce::jlimit (0.0f, 1.0f, proportion));
}

juce::Path PhibresLookAndFeel::chamferedRect (juce::Rectangle<float> area, float cut)
{
    const float c = juce::jmax (0.0f, juce::jmin (cut, area.getWidth() * 0.5f, area.getHeight() * 0.5f));
    const float x = area.getX(), y = area.getY(), r = area.getRight(), b = area.getBottom();

    juce::Path path;
    path.startNewSubPath (x + c, y);
    path.lineTo (r - c, y);
    path.lineTo (r, y + c);
    path.lineTo (r, b - c);
    path.lineTo (r - c, b);
    path.lineTo (x + c, b);
    path.lineTo (x, b - c);
    path.lineTo (x, y + c);
    path.closeSubPath();
    return path;
}

void PhibresLookAndFeel::strokeGlowing (juce::Graphics& g, const juce::Path& path, float thickness,
                                         const juce::ColourGradient& gradient, bool roundedEnds)
{
    struct Layer { float extra, alpha; };

    for (const auto layer : { Layer { 12.0f, 0.07f }, Layer { 8.0f, 0.10f }, Layer { 4.0f, 0.16f } })
    {
        g.setGradientFill (gradient);
        g.setOpacity (layer.alpha);
        g.strokePath (path, juce::PathStrokeType (thickness + layer.extra, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    g.setGradientFill (gradient);
    g.setOpacity (1.0f);
    g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                              roundedEnds ? juce::PathStrokeType::rounded : juce::PathStrokeType::butt));
}

void PhibresLookAndFeel::dropShadow (juce::Graphics& g, const juce::Path& path, float radius, float offsetY, float alpha)
{
    juce::DropShadow (shadow().withAlpha (alpha), juce::roundToInt (radius), { 0, juce::roundToInt (offsetY) }).drawForPath (g, path);
}

void PhibresLookAndFeel::innerShadow (juce::Graphics& g, const juce::Path& shape, float radius, float offsetY, float alpha)
{
    // L'ombre de tout ce qui entoure la forme, vue à travers la forme : sombre le long du bord haut, puis estompée.
    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (shape);

    juce::Path surround;
    surround.setUsingNonZeroWinding (false);
    surround.addRectangle (shape.getBounds().expanded (radius * 3.0f + std::abs (offsetY) + 4.0f));
    surround.addPath (shape);

    juce::DropShadow (shadow().withAlpha (alpha), juce::roundToInt (radius), { 0, juce::roundToInt (offsetY) }).drawForPath (g, surround);
}

void PhibresLookAndFeel::fillVertical (juce::Graphics& g, const juce::Path& path, juce::Rectangle<float> area,
                                        juce::Colour top, juce::Colour bottom)
{
    g.setGradientFill (juce::ColourGradient (top, area.getX(), area.getY(), bottom, area.getX(), area.getBottom(), false));
    g.fillPath (path);
}

void PhibresLookAndFeel::drawScreen (juce::Graphics& g, juce::Rectangle<float> area, float cut)
{
    const auto shape = chamferedRect (area, cut);

    // Lèvre claire sous l'écran, puis l'écran lui-même, en creux.
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.strokePath (shape, juce::PathStrokeType (1.2f), juce::AffineTransform::translation (0.0f, 1.2f));

    fillVertical (g, shape, area, juce::Colour (0xfff2f5f8), juce::Colours::white);
    innerShadow (g, shape, 4.0f, 2.0f, 0.30f);

    g.setColour (juce::Colour (0xffa7b3c2));
    g.strokePath (shape, juce::PathStrokeType (1.0f));
}

juce::Font PhibresLookAndFeel::font (float pointHeight, bool bold, float tracking) const
{
    const juce::Font base (juce::FontOptions (bold ? boldFace : semiBoldFace).withPointHeight (pointHeight));
    return tracking > 0.0f ? base.withExtraKerningFactor (tracking) : base;
}

juce::Font PhibresLookAndFeel::getLabelFont (juce::Label&)
{
    return font (20.0f, true);
}

//==============================================================================
void PhibresLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                            float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

    if (slider.getProperties().contains ("mini"))
        drawMiniKnob (g, bounds, sliderPos, rotaryStartAngle, rotaryEndAngle);
    else
        drawMainKnob (g, bounds, sliderPos, rotaryStartAngle, rotaryEndAngle, slider);
}

void PhibresLookAndFeel::drawMainKnob (juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    const auto centre = bounds.getCentre();
    const float scale = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 190.0f;
    const float arcRadius = 76.0f * scale, tickRadius = 89.0f * scale, capRadius = 44.0f * scale;
    const float bodyRadius = mainKnobBodyRadius (bounds);
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const bool isStandby = (bool) slider.getProperties().getWithDefault ("standby", false);

    // Le dégradé court de gauche (bleu) à droite (cyan) : il suit la progression de l'arc.
    const float left = centre.x - arcRadius;
    const auto gradient = accentGradient ({ left, centre.y }, { centre.x + arcRadius, centre.y });
    const auto accentAtX = [left, arcRadius] (float px) { return accentAt ((px - left) / (2.0f * arcRadius)); };
    const juce::PathStrokeType arcStroke (6.0f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    // Piste en creux : un bord sombre en haut, un bord clair en bas.
    juce::Path trackArc;
    trackArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.strokePath (trackArc, arcStroke, juce::AffineTransform::translation (0.0f, 1.0f));
    g.setColour (juce::Colour (0xffafbac8));
    g.strokePath (trackArc, arcStroke, juce::AffineTransform::translation (0.0f, -0.7f));
    g.setColour (track());
    g.strokePath (trackArc, arcStroke);

    if (angle > startAngle + 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);

        if (isStandby)
        {
            g.setColour (standby());
            g.strokePath (valueArc, arcStroke);
        }
        else
        {
            strokeGlowing (g, valueArc, 6.0f * scale, gradient);
        }
    }

    // Crans pour les potards à pas (divisions du mode sync)
    const int numSteps = slider.getInterval() > 0.0 ? juce::roundToInt ((slider.getMaximum() - slider.getMinimum()) / slider.getInterval()) : 0;

    if (numSteps > 0 && numSteps <= 16)
    {
        for (int i = 0; i <= numSteps; ++i)
        {
            const float a = startAngle + ((float) i / (float) numSteps) * (endAngle - startAngle);
            const auto p = centre.getPointOnCircumference (tickRadius, a);
            const bool lit = a <= angle + 0.001f;

            g.setColour (! lit ? juce::Colour (0xffaeb9c8) : isStandby ? standby() : accentAtX (p.x));
            g.fillEllipse (juce::Rectangle<float> (4.8f * scale, 4.8f * scale).withCentre (p));
        }
    }

    // Corps du potard (son ombre portée est dessinée une fois pour toutes dans le fond)
    const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white, centre.x, body.getY(),
                                             juce::Colour (0xffd3dae4), centre.x, body.getBottom(), false));
    g.fillEllipse (body);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white, centre.x, body.getY(),
                                             juce::Colour (0xff9eaaba), centre.x, body.getBottom(), false));
    g.drawEllipse (body, 1.5f * scale);

    // Calotte : légèrement creusée, avec un reflet en haut
    const auto cap = juce::Rectangle<float> (capRadius * 2.0f, capRadius * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe3e8ef), centre.x, cap.getY(),
                                             juce::Colour (0xfffafbfd), centre.x, cap.getBottom(), false));
    g.fillEllipse (cap);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffb4bfcc), centre.x, cap.getY(),
                                             juce::Colours::white, centre.x, cap.getBottom(), false));
    g.drawEllipse (cap, 1.2f * scale);

    // Repère
    const auto tip = centre.getPointOnCircumference (50.0f * scale, angle);
    juce::Path pointer;
    pointer.startNewSubPath (centre.getPointOnCircumference (24.0f * scale, angle));
    pointer.lineTo (tip);
    const juce::PathStrokeType pointerStroke (4.5f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (shadow().withAlpha (0.18f));
    g.strokePath (pointer, pointerStroke, juce::AffineTransform::translation (0.0f, 1.5f * scale));
    g.setColour (isStandby ? juce::Colour (0xff7c8ba6) : accentAtX (tip.x));
    g.strokePath (pointer, pointerStroke);

    // Valeurs de début et de fin de course
    const auto endText = [&slider] (double proportion)
    {
        return compactValue (slider.getTextFromValue (slider.proportionOfLengthToValue (proportion)));
    };

    g.setColour (dimInk());
    g.setFont (font (10.0f * scale, false));
    g.drawText (endText (0.0), juce::Rectangle<float> (64.0f, 14.0f).withCentre ({ centre.x - 65.0f * scale, centre.y + 82.0f * scale }),
                juce::Justification::centred, false);
    g.drawText (endText (1.0), juce::Rectangle<float> (64.0f, 14.0f).withCentre ({ centre.x + 65.0f * scale, centre.y + 82.0f * scale }),
                juce::Justification::centred, false);
}

void PhibresLookAndFeel::drawMiniKnob (juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos,
                                        float startAngle, float endAngle)
{
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float arcRadius = radius - 3.0f, bodyRadius = miniKnobBodyRadius (bounds);
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const juce::PathStrokeType arcStroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path trackArc;
    trackArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.strokePath (trackArc, arcStroke, juce::AffineTransform::translation (0.0f, 0.8f));
    g.setColour (juce::Colour (0xffc2cbd7));
    g.strokePath (trackArc, arcStroke);

    if (angle > startAngle + 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
        g.setGradientFill (accentGradient ({ centre.x - arcRadius, centre.y }, { centre.x + arcRadius, centre.y }));
        g.strokePath (valueArc, arcStroke);
    }

    const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white, centre.x, body.getY(),
                                             juce::Colour (0xffd5dce6), centre.x, body.getBottom(), false));
    g.fillEllipse (body);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white, centre.x, body.getY(),
                                             juce::Colour (0xff9eaaba), centre.x, body.getBottom(), false));
    g.drawEllipse (body, 1.2f);

    juce::Path pointer;
    pointer.startNewSubPath (centre.getPointOnCircumference (bodyRadius * 0.3f, angle));
    pointer.lineTo (centre.getPointOnCircumference (bodyRadius - 2.0f, angle));
    g.setColour (blue());
    g.strokePath (pointer, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void PhibresLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                bool highlighted, bool down)
{
    const float cut = (float) (double) button.getProperties().getWithDefault ("cut", 8.0);
    const auto area = button.getLocalBounds().toFloat();
    const auto shape = chamferedRect (area, cut);
    const auto inner = chamferedRect (area.reduced (1.5f), cut - 1.0f);

    if (! button.isEnabled())
    {
        // Hors service (variante rythmique en mode free) : à plat, sans relief.
        g.setColour (juce::Colour (0xffcfd6df));
        g.fillPath (shape);
        g.setColour (juce::Colour (0xffe9edf2));
        g.fillPath (inner);
        return;
    }

    if (button.getToggleState())
    {
        // Allumé : le dégradé d'accent, bombé (reflet en haut, ombre en bas).
        g.setGradientFill (accentGradient (area.getTopLeft(), area.getTopRight(), true));
        g.fillPath (shape);

        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.30f), area.getX(), area.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), area.getX(), area.getCentreY(), false));
        g.fillPath (shape);
        g.setGradientFill (juce::ColourGradient (shadow().withAlpha (0.0f), area.getX(), area.getCentreY(),
                                                 shadow().withAlpha (0.22f), area.getX(), area.getBottom(), false));
        g.fillPath (shape);

        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.strokePath (inner, juce::PathStrokeType (1.0f));
    }
    else
    {
        // Éteint : une touche claire en relief, bord sombre, liseré blanc.
        g.setColour (juce::Colour (0xffa3afbe));
        g.fillPath (shape);
        fillVertical (g, chamferedRect (area.reduced (1.0f), cut - 0.7f), area, juce::Colours::white, juce::Colour (0xffe0e5ec));
        g.setColour (juce::Colours::white);
        g.strokePath (inner, juce::PathStrokeType (1.0f));
    }

    if (down)
    {
        g.setColour (shadow().withAlpha (0.14f));
        g.fillPath (shape);
    }
    else if (highlighted)
    {
        g.setColour (juce::Colours::white.withAlpha (button.getToggleState() ? 0.10f : 0.45f));
        g.fillPath (shape);
    }
}

void PhibresLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool down)
{
    const bool on = button.getToggleState();
    const auto& properties = button.getProperties();
    const bool big = properties.contains ("big"), small = properties.contains ("small"), tiny = properties.contains ("tiny");
    const auto text = button.getButtonText();
    const auto textFont = font (big ? 18.0f : small ? 10.5f : tiny ? 9.0f : 13.0f, true, big ? 0.2f : tiny ? 0.08f : 0.12f);
    const auto textColour = ! button.isEnabled() ? juce::Colour (0xffb4bdc9) : on ? juce::Colours::white : juce::Colour (0xff2a3342);
    const auto area = button.getLocalBounds().toFloat().translated (0.0f, down ? 1.0f : 0.0f);   // la touche s'enfonce

    g.setFont (textFont);

    if (! big)
    {
        if (on)   // ombre du texte : il se détache du dégradé
        {
            g.setColour (juce::Colour (0xff0a3a8a).withAlpha (0.45f));
            g.drawText (text, area.translated (0.0f, 1.0f), juce::Justification::centred, false);
        }

        g.setColour (textColour);
        g.drawText (text, area, juce::Justification::centred, false);
        return;
    }

    // Grand bouton : deux barres obliques (témoin) puis le libellé, le tout centré.
    const float iconWidth = 14.0f, gap = 14.0f;
    const float labelWidth = textWidth (textFont, text);
    const float left = area.getCentreX() - (iconWidth + gap + labelWidth) * 0.5f;
    const float midY = area.getCentreY();

    juce::Path bars;
    bars.startNewSubPath (left + 1.0f, midY - 7.0f);
    bars.lineTo (left + 6.0f, midY + 7.0f);
    bars.startNewSubPath (left + 9.0f, midY - 7.0f);
    bars.lineTo (left + 14.0f, midY + 7.0f);

    const auto labelArea = juce::Rectangle<float> (left + iconWidth + gap, area.getY(), labelWidth + 12.0f, area.getHeight());

    if (on)
    {
        g.setColour (juce::Colour (0xff0a3a8a).withAlpha (0.45f));
        g.strokePath (bars, juce::PathStrokeType (3.0f), juce::AffineTransform::translation (0.0f, 1.5f));
        g.drawText (text, labelArea.translated (0.0f, 1.5f), juce::Justification::centredLeft, false);
    }

    g.setColour (on ? juce::Colours::white : juce::Colour (0xffa9b6c8));
    g.strokePath (bars, juce::PathStrokeType (3.0f));

    g.setColour (textColour);
    g.drawText (text, labelArea, juce::Justification::centredLeft, false);
}

//==============================================================================
TapeDisplay::TapeDisplay (PhibresAudioProcessor& p) : audioProcessor (p)
{
    setComponentID ("tape");
    setInterceptsMouseClicks (false, false);
    low.fill (1.0f);     // bas > haut : rien à dessiner
    high.fill (-1.0f);
    startTimerHz (30);
}

void TapeDisplay::refresh()
{
    const auto& engine = audioProcessor.getEngine();
    GrainStretcher::View view;
    bool changed = false;

    {
        // prepareToPlay réalloue les bandes : on ne les lit pas pendant ce temps.
        const juce::ScopedTryLock guard (audioProcessor.getEngineLock());

        if (! guard.isLocked())
            return;

        const auto sequence = engine.getLayoutSeq();

        if (! engine.getView (view))
            return;   // une bande est en cours de remplacement : au prochain passage

        const double loop = juce::jmax (64.0, view.loopLength);
        const int shownUpTo = juce::jmin (view.end, view.origin + (int) std::ceil (loop));

        if (view.running && (sequence != shownSequence || view.captures != shownCaptures || view.origin != shownOrigin
                             || std::abs (loop - shownLoop) > 0.0 || shownUpTo != shownEnd))
        {
            std::array<float, (size_t) numBins> newLow {}, newHigh {};
            const int numChannels = engine.getNumChannels();

            for (int bin = 0; bin < numBins; ++bin)
            {
                const double from = (double) view.origin + loop * (double) bin / numBins;
                const double to = (double) view.origin + loop * (double) (bin + 1) / numBins;
                float lowest = 1.0f, highest = -1.0f;

                if ((int) from < view.end)
                {
                    // Une vingtaine de points par colonne suffisent à l'œil, quelle que soit la longueur de la boucle.
                    const int count = juce::jlimit (1, 24, (int) (to - from));
                    lowest = highest = 0.0f;

                    for (int i = 0; i < count; ++i)
                    {
                        const int index = (int) (from + (to - from) * (double) i / count);

                        if (index < view.begin || index >= view.end)
                            continue;

                        for (int ch = 0; ch < numChannels; ++ch)
                        {
                            const float sample = engine.getTapeData (view.tape, ch)[index];

                            if (std::isfinite (sample))
                            {
                                lowest = juce::jmin (lowest, sample);
                                highest = juce::jmax (highest, sample);
                            }
                        }
                    }
                }

                newLow[(size_t) bin] = lowest;
                newHigh[(size_t) bin] = highest;
            }

            if (engine.getLayoutSeq() != sequence)
                return;   // la bande a changé pendant la lecture : on jette

            low = newLow;
            high = newHigh;
            shownSequence = sequence;
            shownCaptures = view.captures;
            shownOrigin = view.origin;
            shownLoop = loop;
            shownEnd = shownUpTo;
            changed = true;
        }

        const float newRecorded = (float) juce::jlimit (0.0, 1.0, (double) (view.end - view.origin) / loop);
        const float newPlayhead = (float) juce::jlimit (0.0, 1.0, view.playhead);

        changed = changed || view.running != running
                          || (view.running && (std::abs (newPlayhead - playhead) > 1.0e-4f || std::abs (newRecorded - recorded) > 1.0e-4f));

        running = view.running;
        playhead = newPlayhead;
        recorded = newRecorded;
    }

    if (changed)
        repaint();
}

void TapeDisplay::paint (juce::Graphics& g)
{
    // L'écran en creux est dessiné par le fond de l'interface ; ici, seulement la bande.
    const auto plot = plotArea();
    const float midY = plot.getCentreY();
    const float binWidth = plot.getWidth() / (float) numBins;

    if (! running)
    {
        // Rien de capturé : une ligne en pointillé.
        g.setColour (LF::standby().withAlpha (0.8f));
        const float dashes[] = { 3.0f, 4.0f };
        g.drawDashedLine ({ plot.getX(), midY, plot.getRight(), midY }, dashes, 2, 1.2f);
        return;
    }

    // L'échelle suit le niveau de la capture : un son faible reste lisible.
    float peak = 0.05f;

    for (int bin = 0; bin < numBins; ++bin)
        if (low[(size_t) bin] <= high[(size_t) bin])
            peak = juce::jmax (peak, -low[(size_t) bin], high[(size_t) bin]);

    const float scale = 0.5f * plot.getHeight() / peak;
    juce::Path wave;
    int lastRecorded = -1;

    for (int bin = 0; bin < numBins; ++bin)
    {
        if (low[(size_t) bin] > high[(size_t) bin])
            break;

        const float x = plot.getX() + binWidth * (float) bin;
        const float top = midY - juce::jmax (0.6f, high[(size_t) bin] * scale);
        const float bottom = midY + juce::jmax (0.6f, -low[(size_t) bin] * scale);
        wave.addRectangle (x, top, binWidth + 0.3f, bottom - top);
        lastRecorded = bin;
    }

    const float recordedX = plot.getX() + binWidth * (float) (lastRecorded + 1);

    g.setGradientFill (LF::accentGradient (plot.getTopLeft(), plot.getTopRight()));
    g.fillPath (wave);

    if (recordedX < plot.getRight() - 1.0f)   // la suite de la boucle n'est pas encore enregistrée
    {
        g.setColour (LF::standby().withAlpha (0.8f));
        const float dashes[] = { 3.0f, 4.0f };
        g.drawDashedLine ({ recordedX + 2.0f, midY, plot.getRight(), midY }, dashes, 2, 1.2f);
    }

    // Tête de lecture
    const float headX = plot.getX() + playhead * plot.getWidth();
    g.setColour (LF::ink());
    g.fillRect (juce::Rectangle<float> (headX - 0.75f, plot.getY() - 3.0f, 1.5f, plot.getHeight() + 6.0f));

    juce::Path cap;
    cap.addTriangle (headX - 3.5f, plot.getY() - 4.0f, headX + 3.5f, plot.getY() - 4.0f, headX, plot.getY() + 1.0f);
    g.fillPath (cap);
}

//==============================================================================
PhibresFacade::PhibresFacade (PhibresAudioProcessor& p)
    : audioProcessor (p), tapeDisplay (p),
      presetBar (p.presets, [this] (float size, bool bold, float tracking) { return lookAndFeel.font (size, bold, tracking); })
{
    setLookAndFeel (&lookAndFeel);

    using Ids = PhibresAudioProcessor;
    auto& apvts = audioProcessor.apvts;

    // Grands potards : un par module, plusieurs sliders superposés quand le module a plusieurs modes.
    configureMainKnob (lengthTimeKnob, Ids::lengthTimeId, 0);
    configureMainKnob (lengthDivisionKnob, Ids::lengthDivisionId, 0);
    configureMainKnob (stretchFactorKnob, Ids::stretchFactorId, 1);
    configureMainKnob (stretchDivisionKnob, Ids::stretchDivisionId, 1);
    configureMainKnob (grainTimeKnob, Ids::grainTimeId, 2);
    configureMainKnob (grainDivisionKnob, Ids::grainDivisionId, 2);
    configureMainKnob (grainNoteKnob, Ids::grainNoteId, 2);
    configureMainKnob (sprayKnob, Ids::sprayId, 3);
    configureMainKnob (cutoffKnob, Ids::filterCutoffId, 4);
    sprayKnob.setVisible (true);
    cutoffKnob.setVisible (true);

    // Petits potards des panneaux du bas
    configureMiniKnob (positionKnob, Ids::positionId, "POSITION", 0, 0, 3);
    configureMiniKnob (everyKnob, Ids::autoDivisionId, "EVERY", 0, 1, 3);
    configureMiniKnob (pitchKnob, Ids::pitchId, "PITCH", 1, 0, 3);
    configureMiniKnob (sizeKnob, Ids::grainSizeId, "SIZE", 2, 0, 3);
    configureMiniKnob (shapeKnob, Ids::grainShapeId, "SHAPE", 2, 1, 3);
    configureMiniKnob (tiltKnob, Ids::grainTiltId, "TILT", 2, 2, 3);
    configureMiniKnob (detuneKnob, Ids::detuneId, "DETUNE", 3, 0, 3);
    configureMiniKnob (flipChanceKnob, Ids::flipChanceId, "FLIP", 3, 1, 3);
    configureMiniKnob (widthKnob, Ids::widthId, "WIDTH", 3, 2, 3);
    configureMiniKnob (resonanceKnob, Ids::filterResonanceId, "RESO", 4, 0, 3);
    configureMiniKnob (mixKnob, Ids::mixId, "MIX", 4, 1, 3);

    for (int column = 0; column < numColumns; ++column)
        configureValueLabel (valueLabels[column], column);

    // Grands boutons : des bascules. Les lumières de chaque module suivent son état.
    configureBigToggle (stretchButton, Ids::stretchId, 0, [this] (bool on) { stretchOn = on; });
    configureBigToggle (freezeButton, Ids::freezeId, 1, [this] (bool on)
    {
        freezeOn = on;
        setStandby (1, on);   // tête gelée : le facteur d'étirement ne joue plus
    });
    configureBigToggle (flipButton, Ids::flipId, 2, [this] (bool on) { flipOn = on; });
    configureBigToggle (scatterButton, Ids::scatterId, 3, [this] (bool on)
    {
        scatterOn = on;
        setStandby (3, ! on);
    });
    configureBigToggle (filterButton, Ids::filterId, 4, [this] (bool on)
    {
        filterOn = on;
        setStandby (4, ! on);
    });

    // Interrupteurs des baies
    const juce::StringArray freeOrSync { "FREE", "SYNC" };
    configureSwitch (lengthModeSwitch, Ids::lengthModeId, freeOrSync, switchArea (0, modeOffset, modeWidth), false, [this] (int choice) { lengthMode = choice; modesChanged(); });
    configureSwitch (stretchModeSwitch, Ids::stretchModeId, freeOrSync, switchArea (1, modeOffset, modeWidth), false, [this] (int choice) { stretchMode = choice; modesChanged(); });
    configureSwitch (grainModeSwitch, Ids::grainModeId, { "FREE", "SYNC", "NOTE" }, switchArea (2, modeOffset, modeWidth), true, [this] (int choice) { grainMode = choice; modesChanged(); });
    configureSwitch (detuneModeSwitch, Ids::detuneModeId, { "FINE", "SEMI", "OCT" }, switchArea (3, modeOffset, columnWidth - 2 * modeOffset), false, [this] (int choice) { detuneMode = choice; repaint(); });
    configureSwitch (filterTypeSwitch, Ids::filterTypeId, { "HP", "BP", "LP" }, switchArea (4, modeOffset, modeWidth), false, [this] (int choice) { filterType = choice; repaint(); });
    configureSwitch (filterSlopeSwitch, Ids::filterSlopeId, { "12 dB", "24 dB" }, switchArea (4, feelOffset, 2 * feelWidth), true, [this] (int choice) { steepFilter = choice == 1; repaint(); });

    configureFeel (lengthFeel, Ids::lengthFeelId, 0);
    configureFeel (stretchFeel, Ids::stretchFeelId, 1);
    configureFeel (grainFeel, Ids::grainFeelId, 2);

    // --- Panneau CAPTURE : la bande, la position de la tête, la recapture ---
    addAndMakeVisible (tapeDisplay);
    tapeDisplay.setBounds (displayArea (0));

    configureSmallToggle (autoButton, Ids::autoCaptureId, panelKeyArea (0, 0, 2));

    // GRAB : une touche, pas une bascule. Le paramètre vaut 1 tant qu'elle est enfoncée ; la capture part à l'appui.
    addAndMakeVisible (grabButton);
    grabButton.setComponentID (Ids::grabId);
    grabButton.setWantsKeyboardFocus (false);
    grabButton.getProperties().set ("cut", 5.0);
    grabButton.getProperties().set ("tiny", true);
    grabButton.setBounds (panelKeyArea (0, 1, 2));
    grabAttachment = std::make_unique<juce::ParameterAttachment> (*apvts.getParameter (Ids::grabId), [] (float) {});

    grabButton.onStateChange = [this]
    {
        const bool down = grabButton.isDown();

        if (down != grabDown)
        {
            grabDown = down;
            grabAttachment->setValueAsCompleteGesture (down ? 1.0f : 0.0f);
        }
    };

    // --- Panneau PLAYBACK : hauteur des grains et sens de la tête ---
    // Pitch : par demi-tons (le potard se pose sur le demi-ton le plus proche quand on le lâche) ou libre.
    configureSwitch (pitchModeSwitch, Ids::pitchModeId, { "SEMI", "FREE" },
                     panelKeyArea (1, 0, 2).withLeft (columnX (1) + panelInset + panelInnerWidth / 3 + 6), true,
                     [this] (int choice) { pitchInSemitones = choice == 0; repaint(); });
    pitchAttachment = std::make_unique<juce::ParameterAttachment> (*apvts.getParameter (Ids::pitchId), [] (float) {});

    pitchKnob.onDragEnd = [this]
    {
        const auto value = (float) pitchKnob.getValue();

        if (pitchInSemitones && std::abs (value - std::round (value)) > 0.001f)
            pitchAttachment->setValueAsCompleteGesture (std::round (value));
    };

    configureSmallToggle (reverseButton, Ids::reverseId, panelKeyArea (1, 1, 2).withLeft (columnX (1) + panelInset + panelInnerWidth / 3 + 6));
    stateWatchers.push_back (std::make_unique<juce::ParameterAttachment> (*apvts.getParameter (Ids::reverseId), [this] (float value)
    {
        reverseOn = value > 0.5f;
        repaint();
    }));

    // --- Panneau OUTPUT ---
    configureSmallToggle (limiterButton, Ids::limiterId, panelKeyArea (4, 0, 1));

    // Tout est en place : on aligne l'interface sur les paramètres.
    for (auto& watcher : stateWatchers)
        watcher->sendInitialUpdate();

    for (auto* selector : { &lengthModeSwitch, &stretchModeSwitch, &grainModeSwitch, &detuneModeSwitch, &filterTypeSwitch, &filterSlopeSwitch, &pitchModeSwitch })
        selector->attachment->sendInitialUpdate();

    for (auto* feel : { &lengthFeel, &stretchFeel, &grainFeel })
        feel->attachment->sendInitialUpdate();

    setSize (editorWidth, editorHeight);

    // Presets et taille de la fenêtre, dans l'en-tête, à gauche du numéro de version
    addAndMakeVisible (presetBar);
    presetBar.setBounds (editorWidth - 112 - phragma::PresetBar::idealWidth, 26, phragma::PresetBar::idealWidth, 28);
    presetBar.attachSavePanelTo (*this);
    startTimerHz (15);
}

PhibresFacade::~PhibresFacade()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void PhibresFacade::configureMainKnob (juce::Slider& knob, const char* parameterId, int column)
{
    addChildComponent (knob);
    knob.setComponentID (parameterId);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    knob.setWantsKeyboardFocus (false);
    knob.setBounds (knobArea (column));
    knob.onValueChange = [this] { updateValueLabels(); repaint(); };

    sliderAttachments.push_back (std::make_unique<APVTS::SliderAttachment> (audioProcessor.apvts, parameterId, knob));
}

void PhibresFacade::configureMiniKnob (juce::Slider& knob, const char* parameterId, const char* name, int column, int slot, int numSlots)
{
    addAndMakeVisible (knob);
    knob.setName (name);
    knob.setComponentID (parameterId);
    knob.getProperties().set ("mini", true);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    knob.setWantsKeyboardFocus (false);
    knob.setBounds (miniKnobArea (column, slot, numSlots));
    knob.onValueChange = [this] { repaint(); };   // afficheurs et valeurs

    sliderAttachments.push_back (std::make_unique<APVTS::SliderAttachment> (audioProcessor.apvts, parameterId, knob));
}

void PhibresFacade::configureValueLabel (juce::Label& label, int column)
{
    addAndMakeVisible (label);
    label.setComponentID ("value/" + juce::String (column));
    label.setJustificationType (juce::Justification::centredRight);
    label.setMinimumHorizontalScale (0.7f);
    label.setBounds (readoutArea (column).withTrimmedLeft (74).withTrimmedRight (9));

    // Saisie au clavier : la valeur tapée est appliquée au potard affiché dans ce module.
    label.onTextChange = [this, &label, column]
    {
        auto& knob = activeKnob (column);
        knob.setValue (knob.getValueFromText (label.getText()), juce::sendNotificationSync);
        updateValueLabels();
    };
}

void PhibresFacade::configureBigToggle (juce::TextButton& button, const char* parameterId, int column,
                                                      std::function<void (bool)> onChange)
{
    addAndMakeVisible (button);
    button.setComponentID (parameterId);
    button.setWantsKeyboardFocus (false);
    button.getProperties().set ("cut", 16.0);
    button.getProperties().set ("big", true);
    button.setBounds (buttonArea (column));
    button.setClickingTogglesState (true);

    buttonAttachments.push_back (std::make_unique<APVTS::ButtonAttachment> (audioProcessor.apvts, parameterId, button));

    stateWatchers.push_back (std::make_unique<juce::ParameterAttachment> (*audioProcessor.apvts.getParameter (parameterId),
                                                                          [this, onChange] (float value)
                                                                          {
                                                                              onChange (value > 0.5f);
                                                                              repaint();
                                                                          }));
}

void PhibresFacade::configureSmallToggle (juce::TextButton& button, const char* parameterId, juce::Rectangle<int> area)
{
    addAndMakeVisible (button);
    button.setComponentID (parameterId);
    button.setClickingTogglesState (true);
    button.setWantsKeyboardFocus (false);
    button.getProperties().set ("cut", 5.0);
    button.getProperties().set ("tiny", true);
    button.setBounds (area);

    buttonAttachments.push_back (std::make_unique<APVTS::ButtonAttachment> (audioProcessor.apvts, parameterId, button));
}

void PhibresFacade::configureSwitch (ChoiceSwitch& selector, const char* parameterId, const juce::StringArray& texts,
                                                   juce::Rectangle<int> area, bool tiny, std::function<void (int)> onChange)
{
    const int count = texts.size();

    for (int i = 0; i < count; ++i)
    {
        auto* button = selector.buttons.add (new juce::TextButton (texts[i]));
        addAndMakeVisible (*button);

        // Identifiants "paramètre/0", "paramètre/1"… : pour retrouver chaque position (tests, accessibilité).
        button->setComponentID (juce::String (parameterId) + "/" + juce::String (i));
        button->setClickingTogglesState (false);
        button->setWantsKeyboardFocus (false);
        button->getProperties().set ("cut", tiny ? 5.0 : 6.0);
        button->getProperties().set (tiny ? "tiny" : "small", true);

        const int left = area.getX() + (area.getWidth() * i) / count;
        const int right = area.getX() + (area.getWidth() * (i + 1)) / count;
        button->setBounds (left, area.getY(), right - left, area.getHeight());
        button->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < count - 1 ? juce::Button::ConnectedOnRight : 0));
    }

    auto* buttons = &selector.buttons;

    selector.attachment = std::make_unique<juce::ParameterAttachment> (*audioProcessor.apvts.getParameter (parameterId),
                                                                       [buttons, onChange] (float value)
                                                                       {
                                                                           const int chosen = juce::roundToInt (value);

                                                                           for (int i = 0; i < buttons->size(); ++i)
                                                                               buttons->getUnchecked (i)->setToggleState (i == chosen, juce::dontSendNotification);

                                                                           onChange (chosen);
                                                                       });

    auto* attachment = selector.attachment.get();

    for (int i = 0; i < count; ++i)
        selector.buttons.getUnchecked (i)->onClick = [attachment, i] { attachment->setValueAsCompleteGesture ((float) i); };
}

void PhibresFacade::configureFeel (FeelSwitch& feel, const char* parameterId, int column)
{
    for (auto* button : { &feel.triplet, &feel.dotted })
    {
        addAndMakeVisible (*button);
        button->setClickingTogglesState (false);
        button->setWantsKeyboardFocus (false);
        button->getProperties().set ("cut", 6.0);
        button->getProperties().set ("small", true);
    }

    feel.triplet.setComponentID (juce::String (parameterId) + "/1");
    feel.dotted.setComponentID (juce::String (parameterId) + "/2");
    feel.triplet.setConnectedEdges (juce::Button::ConnectedOnRight);
    feel.dotted.setConnectedEdges (juce::Button::ConnectedOnLeft);
    feel.triplet.setBounds (switchArea (column, feelOffset, feelWidth));
    feel.dotted.setBounds (switchArea (column, feelOffset + feelWidth, feelWidth));

    auto* state = &feel;

    feel.attachment = std::make_unique<juce::ParameterAttachment> (*audioProcessor.apvts.getParameter (parameterId),
                                                                   [this, state] (float value)
                                                                   {
                                                                       state->value = juce::roundToInt (value);
                                                                       state->triplet.setToggleState (state->value == 1, juce::dontSendNotification);
                                                                       state->dotted.setToggleState (state->value == 2, juce::dontSendNotification);
                                                                       updateValueLabels();
                                                                   });

    // Un clic sur la variante déjà allumée revient à la division normale.
    feel.triplet.onClick = [state] { state->attachment->setValueAsCompleteGesture (state->value == 1 ? 0.0f : 1.0f); };
    feel.dotted.onClick = [state] { state->attachment->setValueAsCompleteGesture (state->value == 2 ? 0.0f : 2.0f); };
}

juce::Slider& PhibresFacade::activeKnob (int column)
{
    using Ids = PhibresAudioProcessor;

    switch (column)
    {
        case 0:   return lengthMode == Ids::modeSync ? lengthDivisionKnob : lengthTimeKnob;
        case 1:   return stretchMode == Ids::modeSync ? stretchDivisionKnob : stretchFactorKnob;
        case 2:   return grainMode == Ids::modeSync ? grainDivisionKnob : grainMode == Ids::modeNote ? grainNoteKnob : grainTimeKnob;
        case 3:   return sprayKnob;
        default:  return cutoffKnob;
    }
}

bool PhibresFacade::isSynced (int column) const
{
    using Ids = PhibresAudioProcessor;
    return column == 0 ? lengthMode == Ids::modeSync : column == 1 ? stretchMode == Ids::modeSync : column == 2 && grainMode == Ids::modeSync;
}

void PhibresFacade::setStandby (int column, bool standby)
{
    // Module sans effet pour l'instant : son potard passe en veille (il reste réglable) et sa valeur s'estompe.
    for (auto* knob : { &stretchFactorKnob, &stretchDivisionKnob, &sprayKnob, &cutoffKnob })
        if (knob->getBounds() == knobArea (column))
            knob->getProperties().set ("standby", standby);

    valueLabels[column].setColour (juce::Label::textColourId, standby ? LF::dimInk() : LF::ink());
}

void PhibresFacade::modesChanged()
{
    using Ids = PhibresAudioProcessor;

    lengthTimeKnob.setVisible (lengthMode != Ids::modeSync);
    lengthDivisionKnob.setVisible (lengthMode == Ids::modeSync);
    stretchFactorKnob.setVisible (stretchMode != Ids::modeSync);
    stretchDivisionKnob.setVisible (stretchMode == Ids::modeSync);
    grainTimeKnob.setVisible (grainMode == Ids::modeFree);
    grainDivisionKnob.setVisible (grainMode == Ids::modeSync);
    grainNoteKnob.setVisible (grainMode == Ids::modeNote);

    // Les durées libres, les notes et les fréquences se tapent au clavier ; les divisions se choisissent au potard.
    for (int column = 0; column < numColumns; ++column)
        valueLabels[column].setEditable (! isSynced (column), false, false);

    // Triolet / pointé n'ont de sens que sur une division.
    FeelSwitch* const feels[] = { &lengthFeel, &stretchFeel, &grainFeel };

    for (int column = 0; column < 3; ++column)
    {
        feels[column]->triplet.setEnabled (isSynced (column));
        feels[column]->dotted.setEnabled (isSynced (column));
    }

    updateValueLabels();
    repaint();
}

void PhibresFacade::updateValueLabels()
{
    const FeelSwitch* const feels[] = { &lengthFeel, &stretchFeel, &grainFeel };

    for (int column = 0; column < numColumns; ++column)
    {
        auto& knob = activeKnob (column);
        auto text = knob.getTextFromValue (knob.getValue());

        if (column < 3 && isSynced (column))   // "1/16 T" = triolet, "1/16 D" = pointée
            text += feels[column]->value == 1 ? " T" : feels[column]->value == 2 ? " D" : "";

        valueLabels[column].setText (text, juce::dontSendNotification);
    }
}

void PhibresFacade::timerCallback()
{
    // Durées affichées dans le panneau PLAYBACK : elles suivent le tempo de l'hôte.
    const float loopSeconds = audioProcessor.getLoopSeconds(), passSeconds = audioProcessor.getPassSeconds();

    if (std::abs (loopSeconds - shownLoopSeconds) > 1.0e-4f || std::abs (passSeconds - shownPassSeconds) > 1.0e-4f)
    {
        shownLoopSeconds = loopSeconds;
        shownPassSeconds = passSeconds;
        repaint (displayArea (1));
    }
}

void PhibresFacade::resized() {}

//==============================================================================
void PhibresFacade::paint (juce::Graphics& g)
{
    // Le fond (plaques, ombres, textes fixes) est coûteux à dessiner : on le garde en image,
    // à la résolution réelle de l'écran, et on ne redessine à chaque fois que ce qui bouge.
    const float scale = juce::jlimit (0.5f, 4.0f, (float) g.getInternalContext().getPhysicalPixelScaleFactor());

    if (! background.isValid() || std::abs (scale - backgroundScale) > 0.01f)
    {
        backgroundScale = scale;
        background = juce::Image (juce::Image::ARGB, juce::roundToInt ((float) editorWidth * scale),
                                  juce::roundToInt ((float) editorHeight * scale), true);

        juce::Graphics layer (background);
        layer.addTransform (juce::AffineTransform::scale (scale));
        paintBackground (layer);
    }

    g.drawImage (background, getLocalBounds().toFloat());

    paintLights (g);
    paintModules (g);
}

void PhibresFacade::paintBackground (juce::Graphics& g)
{
    const auto full = juce::Rectangle<float> (0.0f, 0.0f, (float) editorWidth, (float) editorHeight);
    const float w = (float) editorWidth, mid = w * 0.5f;

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe2e7ee), 0.0f, 0.0f, juce::Colour (0xffcbd3dd), 0.0f, (float) editorHeight, false));
    g.fillRect (full);

    // Plaque principale : posée sur le fond, avec un liseré clair le long du bord.
    const auto plateArea = full.reduced (10.0f);
    const auto plate = LF::chamferedRect (plateArea, 18.0f);
    LF::dropShadow (g, plate, 12.0f, 5.0f, 0.38f);
    LF::fillVertical (g, plate, plateArea, juce::Colour (0xfffcfdfe), juce::Colour (0xffeceff3));
    g.setColour (juce::Colour (0xffa9b4c2));
    g.strokePath (plate, juce::PathStrokeType (1.3f));
    g.setColour (juce::Colours::white);
    g.strokePath (LF::chamferedRect (plateArea.reduced (1.8f), 17.0f), juce::PathStrokeType (1.2f));
    g.setColour (juce::Colour (0xffdde2e9));
    g.strokePath (LF::chamferedRect (plateArea.reduced (6.0f), 15.0f), juce::PathStrokeType (1.0f));

    // Joints de l'en-tête et du pied : une rainure (trait sombre, reflet clair dessous)
    const auto headerSeam = polyline ({ { 16.0f, 64.0f }, { mid - 80.0f, 64.0f }, { mid - 68.0f, 76.0f },
                                        { mid + 68.0f, 76.0f }, { mid + 80.0f, 64.0f }, { w - 16.0f, 64.0f } });
    const auto footerSeam = polyline ({ { 16.0f, (float) footerY }, { w - 16.0f, (float) footerY } });

    for (const auto* seam : { &headerSeam, &footerSeam })
    {
        g.setColour (juce::Colours::white);
        g.strokePath (*seam, juce::PathStrokeType (1.2f), juce::AffineTransform::translation (0.0f, 1.2f));
        g.setColour (juce::Colour (0xffb9c3d0));
        g.strokePath (*seam, juce::PathStrokeType (1.2f));
    }

    static const char* const panelTitles[numColumns] = { "TAPE", "PLAYBACK", "GRAIN WINDOW", "SCATTER", "OUTPUT" };

    for (int column = 0; column < numColumns; ++column)
    {
        // Baie du potard : en creux dans la plaque.
        const auto bay = bayArea (column).toFloat();
        const auto bayShape = LF::chamferedRect (bay, 22.0f);
        g.setColour (juce::Colours::white);
        g.strokePath (bayShape, juce::PathStrokeType (1.4f), juce::AffineTransform::translation (0.0f, 1.4f));
        LF::fillVertical (g, bayShape, bay, juce::Colour (0xffdde2e9), juce::Colour (0xffebeef3));
        LF::innerShadow (g, bayShape, 10.0f, 4.0f, 0.34f);
        g.setColour (juce::Colour (0xffa9b4c2));
        g.strokePath (bayShape, juce::PathStrokeType (1.2f));

        // Ombre portée du potard : une large et douce, une courte au contact.
        const auto knob = knobArea (column).toFloat();
        const auto body = circle (knob.getCentre(), LF::mainKnobBodyRadius (knob));
        LF::dropShadow (g, body, 13.0f, 9.0f, 0.30f);
        LF::dropShadow (g, body, 3.0f, 2.0f, 0.30f);

        // Afficheur sous le potard (son libellé dépend du mode : il est dessiné avec le reste de ce qui bouge)
        LF::drawScreen (g, readoutArea (column).toFloat(), 8.0f);

        // Panneau du bas : posé sur la plaque.
        const auto panel = panelArea (column).toFloat();
        const auto panelShape = LF::chamferedRect (panel, 14.0f);
        LF::dropShadow (g, panelShape, 8.0f, 4.0f, 0.22f);
        LF::fillVertical (g, panelShape, panel, juce::Colour (0xfffbfcfd), juce::Colour (0xffe8ecf1));
        g.setColour (juce::Colour (0xffb1bcc9));
        g.strokePath (panelShape, juce::PathStrokeType (1.1f));
        g.setColour (juce::Colours::white);
        g.strokePath (LF::chamferedRect (panel.reduced (1.5f), 13.0f), juce::PathStrokeType (1.1f));

        g.setColour (LF::dimInk());
        g.setFont (lookAndFeel.font (9.0f, false, 0.12f));
        g.drawText (panelTitles[column], panelArea (column).withTrimmedLeft (panelInset).withHeight (28), juce::Justification::centredLeft, false);

        // Écran du panneau
        LF::drawScreen (g, displayArea (column).toFloat(), 6.0f);
    }

    // Ombres portées des boutons et des petits potards, noms des petits potards
    for (auto* child : getChildren())
    {
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
        {
            const bool big = button->getProperties().contains ("big");
            const float cut = (float) (double) button->getProperties().getWithDefault ("cut", 8.0);
            const auto shape = LF::chamferedRect (button->getBounds().toFloat(), cut);

            if (big)
                LF::dropShadow (g, shape, 9.0f, 5.0f, 0.30f);

            LF::dropShadow (g, shape, big ? 3.0f : 2.0f, big ? 2.0f : 1.6f, big ? 0.30f : 0.34f);
        }
        else if (auto* knob = dynamic_cast<juce::Slider*> (child))
        {
            if (! knob->getProperties().contains ("mini"))
                continue;

            const auto bounds = knob->getBounds().toFloat();
            LF::dropShadow (g, circle (bounds.getCentre(), LF::miniKnobBodyRadius (bounds)), 4.0f, 3.0f, 0.34f);

            g.setColour (LF::dimInk());
            g.setFont (lookAndFeel.font (9.0f, false, 0.1f));
            g.drawText (knob->getName(), juce::Rectangle<int> (66, 12).withCentre ({ knob->getBounds().getCentreX(), knob->getBottom() + 8 }),
                        juce::Justification::centred, false);
        }
    }

    // En-tête : nom du plugin, signature, et numéro de version à droite
    const auto titleFont = lookAndFeel.font (20.0f, true, 0.24f);
    const auto titleArea = juce::Rectangle<float> (36.0f, 16.0f, 420.0f, 48.0f);

    g.setColour (LF::ink());
    g.setFont (titleFont);
    g.drawText ("PHIBRES", titleArea, juce::Justification::centredLeft, false);

    // La signature est posée sur la même ligne de base que le nom (d'où le décalage vers le bas).
    g.setColour (LF::dimInk());
    g.setFont (lookAndFeel.font (11.0f, false, 0.26f));
    g.drawText ("BY PHRAGMA", titleArea.withTrimmedLeft (textWidth (titleFont, "PHIBRES") + 18.0f).translated (0.0f, 1.5f),
                juce::Justification::centredLeft, false);

    // Numéro de version : pour vérifier d'un coup d'œil quelle version le DAW a chargée
    g.setFont (lookAndFeel.font (11.0f, false, 0.2f));
    g.drawText ("V" PHIBRES_VERSION, juce::Rectangle<float> ((float) editorWidth - 336.0f, 16.0f, 300.0f, 48.0f),
                juce::Justification::centredRight, false);
}

void PhibresFacade::paintLights (juce::Graphics& g)
{
    const bool moduleOn[numColumns] = { stretchOn, freezeOn, flipOn, scatterOn, filterOn };
    const float pipY = (float) footerY + 11.0f;

    // Trait éteint : une rainure (reflet clair dessous).
    const auto strokeUnlit = [&g] (const juce::Path& path, const juce::PathStrokeType& stroke)
    {
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.strokePath (path, stroke, juce::AffineTransform::translation (0.0f, 1.2f));
        g.setColour (LF::unlit());
        g.strokePath (path, stroke);
    };

    for (int column = 0; column < numColumns; ++column)
    {
        const float x = (float) columnX (column);
        const float right = x + (float) columnWidth;

        // Néons d'angle de la baie, et voyants du pied
        const Pt leftStart { x + 13.0f, 150.0f }, leftEnd { x + 72.0f, 97.0f };
        const Pt rightStart { right - 13.0f, 150.0f }, rightEnd { right - 72.0f, 97.0f };
        const auto leftStrip = polyline ({ leftStart, { x + 13.0f, 112.0f }, { x + 28.0f, 97.0f }, leftEnd });
        const auto rightStrip = polyline ({ rightStart, { right - 13.0f, 112.0f }, { right - 28.0f, 97.0f }, rightEnd });

        juce::Path pips;
        pips.startNewSubPath (x + 12.0f, pipY);
        pips.lineTo (x + 24.0f, pipY);
        pips.startNewSubPath (x + 32.0f, pipY);
        pips.lineTo (x + 44.0f, pipY);

        const juce::PathStrokeType pipStroke (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType stripStroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        if (moduleOn[column])
        {
            LF::strokeGlowing (g, leftStrip, 3.0f, LF::accentGradient (leftStart, leftEnd));
            LF::strokeGlowing (g, rightStrip, 3.0f, LF::accentGradient (rightStart, rightEnd));
            LF::strokeGlowing (g, pips, 2.5f, LF::accentGradient ({ x + 12.0f, pipY }, { x + 44.0f, pipY }));

            const auto button = buttonArea (column).toFloat();
            drawHalo (g, LF::chamferedRect (button, 16.0f), LF::accentGradient (button.getTopLeft(), button.getTopRight(), true));
        }
        else
        {
            strokeUnlit (leftStrip, stripStroke);
            strokeUnlit (rightStrip, stripStroke);
            strokeUnlit (pips, pipStroke);
        }
    }

    // Barres obliques du haut : allumées quand l'étirement tourne
    const float mid = (float) editorWidth * 0.5f;
    juce::Path leftBars, rightBars;

    for (const float offset : { 46.0f, 34.0f, 22.0f })
    {
        leftBars.startNewSubPath (mid - offset, 30.0f);
        leftBars.lineTo (mid - offset + 9.0f, 58.0f);
        rightBars.startNewSubPath (mid + offset, 30.0f);
        rightBars.lineTo (mid + offset - 9.0f, 58.0f);
    }

    if (stretchOn)
    {
        LF::strokeGlowing (g, leftBars, 4.0f, LF::accentGradient ({ mid - 46.0f, 44.0f }, { mid - 13.0f, 44.0f }), false);
        LF::strokeGlowing (g, rightBars, 4.0f, LF::accentGradient ({ mid + 46.0f, 44.0f }, { mid + 13.0f, 44.0f }), false);
    }
    else
    {
        strokeUnlit (leftBars, juce::PathStrokeType (4.0f));
        strokeUnlit (rightBars, juce::PathStrokeType (4.0f));
    }
}

void PhibresFacade::paintModules (juce::Graphics& g)
{
    using Ids = PhibresAudioProcessor;

    // --- Libellés des afficheurs : ils disent ce que règle le grand potard dans le mode choisi ---
    {
        const juce::String names[numColumns] = {
            "LENGTH",
            stretchMode == Ids::modeSync ? "OVER" : "FACTOR",
            grainMode == Ids::modeNote ? juce::String (Ids::noteToHertz (juce::roundToInt (grainNoteKnob.getValue())), 1) + " HZ" : juce::String ("RATE"),
            "SPRAY",
            "CUTOFF" };

        g.setColour (LF::dimInk());
        g.setFont (lookAndFeel.font (10.0f, false, 0.12f));

        for (int column = 0; column < numColumns; ++column)
            g.drawText (names[column], readoutArea (column).withTrimmedLeft (13), juce::Justification::centredLeft, false);
    }

    // --- Lecture : durée de la boucle, et durée qu'elle prend une fois étirée ---
    {
        const auto area = displayArea (1).toFloat().reduced (10.0f, 6.0f);
        const float loopSeconds = audioProcessor.getLoopSeconds(), passSeconds = audioProcessor.getPassSeconds();
        const auto textFont = lookAndFeel.font (12.5f, true);
        const juce::String from = durationText (loopSeconds);
        const juce::String to = freezeOn || passSeconds <= 0.0f ? juce::String ("FROZEN") : durationText (passSeconds);

        g.setFont (textFont);
        g.setColour (LF::ink());
        g.drawText (from, area, juce::Justification::centredLeft, false);
        g.setColour (freezeOn ? LF::dimInk() : LF::ink());
        g.drawText (to, area, juce::Justification::centredRight, false);

        // Flèche entre les deux : elle pointe dans le sens où va la tête.
        const float left = area.getX() + textWidth (textFont, from) + 10.0f;
        const float right = area.getRight() - textWidth (textFont, to) - 10.0f;
        const float y = area.getCentreY();

        if (right - left > 14.0f)
        {
            juce::Path arrow;
            arrow.startNewSubPath (left, y);
            arrow.lineTo (right, y);

            if (reverseOn)
            {
                arrow.startNewSubPath (left + 6.0f, y - 4.5f);
                arrow.lineTo (left, y);
                arrow.lineTo (left + 6.0f, y + 4.5f);
            }
            else
            {
                arrow.startNewSubPath (right - 6.0f, y - 4.5f);
                arrow.lineTo (right, y);
                arrow.lineTo (right - 6.0f, y + 4.5f);
            }

            const juce::PathStrokeType stroke (1.8f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded);

            if (freezeOn)
            {
                g.setColour (LF::standby());
                g.strokePath (arrow, stroke);
            }
            else
            {
                g.setGradientFill (LF::accentGradient ({ left, y }, { right, y }));
                g.strokePath (arrow, stroke);
            }
        }
    }

    // --- Grains : la part de chacun (traits fins) et leur somme (courbe pleine), sur quatre périodes, mises à niveau comme le fait le moteur ---
    {
        const auto plot = displayArea (2).toFloat().reduced (9.0f, 7.0f);
        const double size = sizeKnob.getValue() * 0.01, shape = shapeKnob.getValue() * 0.01, tilt = tiltKnob.getValue() * 0.01;
        const double periods = 4.0;
        const int numPoints = 176, numShown = 4;

        const auto xFor = [plot, periods] (double t) { return plot.getX() + (float) (t / periods) * plot.getWidth(); };
        const auto yFor = [plot] (double level) { return plot.getBottom() - (float) juce::jlimit (0.0, 1.0, level / 1.15) * plot.getHeight(); };

        juce::Path outlines[numShown], sum;

        for (int i = 0; i <= numPoints; ++i)
        {
            const double t = periods * (double) i / numPoints;
            double total = 0.0;

            for (int grain = -8; grain < numShown; ++grain)
                total += windowShape ((t - (double) grain) / size, shape, tilt);

            // Le moteur divise par la somme des fenêtres dès qu'elle dépasse 1.
            const double norm = total > 1.0 ? 1.0 / total : 1.0;

            for (int grain = 0; grain < numShown; ++grain)
            {
                const Pt point { xFor (t), yFor (windowShape ((t - (double) grain) / size, shape, tilt) * norm) };

                if (i == 0)
                    outlines[grain].startNewSubPath (point);
                else
                    outlines[grain].lineTo (point);
            }

            const Pt point { xFor (t), yFor (total * norm) };

            if (i == 0)
                sum.startNewSubPath (point);
            else
                sum.lineTo (point);
        }

        g.setColour (LF::dimInk().withAlpha (0.42f));

        for (const auto& outline : outlines)
            g.strokePath (outline, juce::PathStrokeType (1.0f));

        plotCurve (g, plot, sum, 1.8f);
    }

    // --- Dispersion : un nuage de grains. Horizontal = position, vertical = hauteur, couleur = panoramique, anneau = à l'envers ---
    {
        const auto plot = displayArea (3).toFloat().reduced (10.0f, 7.0f);
        const float sprayAmount = std::sqrt ((float) (sprayKnob.getValue() / Ids::maxSprayMs));
        const float detuneAmount = std::sqrt ((float) (detuneKnob.getValue() / Ids::maxDetune));
        const float flipAmount = (float) flipChanceKnob.getValue() * 0.01f, widthAmount = (float) widthKnob.getValue() * 0.01f;
        juce::Random random (20261011);

        for (int i = 0; i < 34; ++i)
        {
            const float a = random.nextFloat() * 2.0f - 1.0f, c = random.nextFloat(), d = random.nextFloat() * 2.0f - 1.0f;
            float b = random.nextFloat() * 2.0f - 1.0f;

            if (detuneMode == GrainStretcher::detuneSemitones)
                b = std::round (b * 6.0f) / 6.0f;      // des paliers
            else if (detuneMode == GrainStretcher::detuneOctaves)
                b = std::round (b * 1.49f) / 1.49f;    // trois lignes : en dessous, à la hauteur, au-dessus

            const Pt centre { plot.getCentreX() + a * sprayAmount * 0.5f * plot.getWidth(),
                              plot.getCentreY() - b * detuneAmount * 0.5f * plot.getHeight() };
            const auto dot = juce::Rectangle<float> (4.4f, 4.4f).withCentre (centre);

            g.setColour (scatterOn ? LF::accentAt (0.5f + 0.5f * d * widthAmount).withAlpha (0.85f) : LF::standby().withAlpha (0.75f));

            if (c < flipAmount)
                g.drawEllipse (dot.reduced (0.5f), 1.2f);
            else
                g.fillEllipse (dot);
        }
    }

    // --- Filtre : courbe de réponse, de 20 Hz à 20 kHz ---
    {
        const auto plot = displayArea (4).toFloat().reduced (9.0f, 8.0f);
        const double cutoff = cutoffKnob.getValue();
        const double quality = OutputFilter::qualityFor (resonanceKnob.getValue() * 0.01);
        const int numPoints = 110;
        juce::Path curve;

        for (int i = 0; i <= numPoints; ++i)
        {
            const double position = (double) i / numPoints;
            const double frequency = OutputFilter::minFrequency * std::pow (OutputFilter::maxFrequency / OutputFilter::minFrequency, position);
            const double decibels = 20.0 * std::log10 (juce::jmax (1.0e-5, OutputFilter::magnitudeAt (frequency, cutoff, quality, filterType, steepFilter)));
            const double level = juce::jlimit (0.0, 1.0, (decibels + 36.0) / 56.0);   // de -36 dB (bas) à +20 dB (haut)
            const Pt point { plot.getX() + (float) position * plot.getWidth(), plot.getBottom() - (float) level * plot.getHeight() };

            if (i == 0)
                curve.startNewSubPath (point);
            else
                curve.lineTo (point);
        }

        plotCurve (g, plot, curve);
    }

    // Valeurs des petits potards (leurs noms sont dans le fond)
    g.setColour (LF::ink());
    g.setFont (lookAndFeel.font (10.5f, true));

    for (auto* knob : { &positionKnob, &everyKnob, &pitchKnob, &sizeKnob, &shapeKnob, &tiltKnob, &detuneKnob, &flipChanceKnob, &widthKnob, &resonanceKnob, &mixKnob })
    {
        // En mode demi-tons, le pitch affiché est celui qu'on entend : arrondi au demi-ton.
        const double value = (knob == &pitchKnob && pitchInSemitones) ? std::round (knob->getValue()) : knob->getValue();

        g.drawText (knob->getTextFromValue (value),
                    juce::Rectangle<int> (70, 12).withCentre ({ knob->getBounds().getCentreX(), knob->getBottom() + 20 }),
                    juce::Justification::centred, false);
    }
}

//==============================================================================
PhibresAudioProcessorEditor::PhibresAudioProcessorEditor (PhibresAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), facade (p)
{
    addAndMakeVisible (facade);

    // Taille gardée avec le projet (lue avant de régler les limites : les régler redimensionne déjà la fenêtre).
    const int savedWidth = (int) audioProcessor.apvts.state.getProperty ("editorWidth", PhibresFacade::width);

    // Fenêtre redimensionnable de 60 % à 150 %, proportions gardées (coin en bas à droite ou menu de taille).
    setResizable (true, true);
    setResizeLimits (juce::roundToInt (PhibresFacade::width * 0.6), juce::roundToInt (PhibresFacade::height * 0.6),
                     juce::roundToInt (PhibresFacade::width * 1.5), juce::roundToInt (PhibresFacade::height * 1.5));
    getConstrainer()->setFixedAspectRatio ((double) PhibresFacade::width / (double) PhibresFacade::height);

    facade.getPresetBar().getScale = [this] { return getUiScale(); };
    facade.getPresetBar().setScale = [this] (float scale) { setUiScale (scale); };

    const int shownWidth = juce::jlimit (juce::roundToInt (PhibresFacade::width * 0.6), juce::roundToInt (PhibresFacade::width * 1.5), savedWidth);
    setSize (shownWidth, juce::roundToInt ((double) shownWidth * PhibresFacade::height / PhibresFacade::width));
    sized = true;
    resized();
}

PhibresAudioProcessorEditor::~PhibresAudioProcessorEditor() = default;

void PhibresAudioProcessorEditor::setUiScale (float scale)
{
    scale = juce::jlimit (0.6f, 1.5f, scale);
    setSize (juce::roundToInt ((float) PhibresFacade::width * scale), juce::roundToInt ((float) PhibresFacade::height * scale));
}

void PhibresAudioProcessorEditor::resized()
{
    facade.setTransform (juce::AffineTransform::scale (getUiScale()));
    facade.getPresetBar().refresh();

    if (sized)
        audioProcessor.apvts.state.setProperty ("editorWidth", getWidth(), nullptr);
}
