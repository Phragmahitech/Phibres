#pragma once

#include "PresetLibrary.h"

/**
    Barre de l'en-tête commune à Phragments, Phibres et Phractal :

        ‹  NOM ▾  ›  SAVE  100 %

    ‹ › : preset précédent / suivant (DEFAULT puis presets utilisateur, en boucle).
    Clic sur le nom : menu (presets, SAVE AS…, DELETE, SHOW PRESETS FOLDER).
    SAVE : panneau dessiné dans la façade (pas de fenêtre séparée dans le DAW).
    100 % : taille de la fenêtre (60 à 150 %) ; on peut aussi tirer le coin en bas à droite.

    Le fichier est identique dans les trois plugins ; il ne dépend que de la police du plugin.
*/
namespace phragma
{

struct PresetPalette
{
    static juce::Colour ink()        { return juce::Colour (0xff101622); }
    static juce::Colour dimInk()     { return juce::Colour (0xff5b6676); }
    static juce::Colour line()       { return juce::Colour (0xffb4becb); }
    static juce::Colour blue()       { return juce::Colour (0xff1785ff); }
    static juce::Colour cyan()       { return juce::Colour (0xff1de4f5); }

    static juce::Path chamfered (juce::Rectangle<float> r, float cut)
    {
        cut = juce::jmin (cut, r.getWidth() * 0.5f, r.getHeight() * 0.5f);
        juce::Path p;
        p.startNewSubPath (r.getX() + cut, r.getY());
        p.lineTo (r.getRight() - cut, r.getY());
        p.lineTo (r.getRight(), r.getY() + cut);
        p.lineTo (r.getRight(), r.getBottom() - cut);
        p.lineTo (r.getRight() - cut, r.getBottom());
        p.lineTo (r.getX() + cut, r.getBottom());
        p.lineTo (r.getX(), r.getBottom() - cut);
        p.lineTo (r.getX(), r.getY() + cut);
        p.closeSubPath();
        return p;
    }
};

using FontMaker = std::function<juce::Font (float height, bool bold, float tracking)>;

//==============================================================================
/** Dessin des boutons de la barre, du menu et du champ du nom. */
class PresetLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    explicit PresetLookAndFeel (FontMaker f) : font (std::move (f))
    {
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xfffbfcfd));
        setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
    }

    FontMaker font;

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        using P = PresetPalette;
        const auto id = b.getComponentID();
        const auto area = b.getLocalBounds().toFloat().reduced (0.5f);
        const auto shape = P::chamfered (area, 5.0f);
        const bool accent = id == "presetConfirm";

        if (accent)
        {
            juce::ColourGradient grad (P::blue(), area.getX(), 0.0f, P::cyan(), area.getRight(), 0.0f, false);
            grad.addColour (0.6, P::blue());
            g.setGradientFill (grad);
            g.fillPath (shape);
            if (down)
            {
                g.setColour (juce::Colours::black.withAlpha (0.12f));
                g.fillPath (shape);
            }
            g.setColour (P::blue().darker (0.3f));
            g.strokePath (shape, juce::PathStrokeType (1.0f));
            return;
        }

        const bool name = id == "presetName";
        g.setGradientFill (juce::ColourGradient (name ? juce::Colour (0xffeef1f5) : juce::Colours::white, 0.0f, area.getY(),
                                                 name ? juce::Colour (0xfff9fafc) : juce::Colour (0xffe9edf2), 0.0f, area.getBottom(), false));
        g.fillPath (shape);

        if (down)
        {
            g.setColour (juce::Colour (0x141b2b4b));
            g.fillPath (shape);
        }

        g.setColour (over || down ? P::blue() : P::line());
        g.strokePath (shape, juce::PathStrokeType (over || down ? 1.3f : 1.1f));
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool down) override
    {
        using P = PresetPalette;
        const auto id = b.getComponentID();
        const auto area = b.getLocalBounds().toFloat();
        const auto colour = b.isEnabled() ? P::ink() : P::dimInk().withAlpha (0.5f);

        if (id == "presetPrev" || id == "presetNext")
        {
            const float dir = id == "presetPrev" ? -1.0f : 1.0f;
            const auto c = area.getCentre();
            juce::Path chevron;
            chevron.startNewSubPath (c.x - dir * 2.5f, c.y - 5.0f);
            chevron.lineTo (c.x + dir * 2.5f, c.y);
            chevron.lineTo (c.x - dir * 2.5f, c.y + 5.0f);
            g.setColour (over || down ? P::blue() : P::ink());
            g.strokePath (chevron, juce::PathStrokeType (1.8f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
            return;
        }

        if (id == "presetName")
        {
            auto textArea = area.reduced (10.0f, 0.0f).withTrimmedRight (14.0f);
            auto text = b.getButtonText();
            g.setFont (font (11.0f, true, 0.08f));

            // L'étoile « modifié » reste visible même quand le nom est coupé
            if (text.endsWith (" *"))
            {
                text = text.dropLastCharacters (2);
                g.setColour (P::blue());
                g.setFont (font (15.0f, true, 0.0f));
                g.drawText ("*", textArea.removeFromRight (10.0f).translated (0.0f, 2.0f), juce::Justification::centredRight, false);
                g.setFont (font (11.0f, true, 0.08f));
                textArea.removeFromRight (4.0f);
            }

            g.setColour (P::ink());
            g.drawText (text, textArea, juce::Justification::centredLeft, true);

            juce::Path caret;
            const float x = area.getRight() - 15.0f, y = area.getCentreY() - 1.5f;
            caret.addTriangle (x, y, x + 7.0f, y, x + 3.5f, y + 4.0f);
            g.setColour (over ? P::blue() : P::dimInk());
            g.fillPath (caret);
            return;
        }

        g.setColour (id == "presetConfirm" ? juce::Colours::white : colour);
        g.setFont (font (10.0f, true, 0.2f));
        g.drawText (b.getButtonText(), area, juce::Justification::centred, false);
    }

    // Menu
    juce::Font getPopupMenuFont() override                  { return font (12.0f, true, 0.06f); }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (juce::Colour (0xfffbfcfd));
        g.setColour (PresetPalette::line());
        g.drawRect (0, 0, w, h, 1);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool, const juce::String& text,
                            const juce::String&, const juce::Drawable*, const juce::Colour*) override
    {
        using P = PresetPalette;

        if (isSeparator)
        {
            g.setColour (juce::Colour (0xffdde2e9));
            g.fillRect (area.reduced (10, 0).withHeight (1).withY (area.getCentreY()));
            return;
        }

        auto r = area.reduced (3, 1);
        if (isHighlighted && isActive)
        {
            juce::ColourGradient grad (P::blue(), (float) r.getX(), 0.0f, P::cyan(), (float) r.getRight(), 0.0f, false);
            grad.addColour (0.7, P::blue());
            g.setGradientFill (grad);
            g.fillRect (r);
        }

        if (isTicked)
        {
            g.setColour (isHighlighted ? juce::Colours::white : P::blue());
            g.fillRect (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ (float) r.getX() + 12.0f, (float) r.getCentreY() }));
        }

        g.setColour (! isActive ? P::dimInk().withAlpha (0.6f) : isHighlighted ? juce::Colours::white : P::ink());
        g.setFont (getPopupMenuFont());
        g.drawText (text, r.withTrimmedLeft (24).withTrimmedRight (10), juce::Justification::centredLeft, true);
    }

    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName) override
    {
        g.setColour (PresetPalette::dimInk());
        g.setFont (font (9.5f, true, 0.22f));
        g.drawText (sectionName, area.withTrimmedLeft (12).withTrimmedTop (4), juce::Justification::centredLeft, false);
    }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& w, int& h) override
    {
        if (isSeparator) { w = 60; h = 9; return; }
        h = 24;
        w = juce::jmax (170, (int) std::ceil (juce::GlyphArrangement::getStringWidth (getPopupMenuFont(), text)) + 44);
    }

    // Champ du nom
    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&) override
    {
        g.setColour (juce::Colours::white);
        g.fillRect (0, 0, w, h);
    }

    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& editor) override
    {
        g.setColour (editor.hasKeyboardFocus (true) ? PresetPalette::blue() : PresetPalette::line());
        g.drawRect (0, 0, w, h, editor.hasKeyboardFocus (true) ? 2 : 1);
    }
};

//==============================================================================
/** Panneau « SAVE PRESET » posé sur toute la façade. */
class SavePresetPanel final : public juce::Component
{
public:
    SavePresetPanel (PresetLookAndFeel& l, PresetLibrary& lib) : lf (l), library (lib)
    {
        setLookAndFeel (&lf);
        setInterceptsMouseClicks (true, true);

        nameField.setFont (lf.font (15.0f, true, 0.04f));
        nameField.setColour (juce::TextEditor::textColourId, PresetPalette::ink());
        nameField.setColour (juce::TextEditor::highlightColourId, PresetPalette::blue().withAlpha (0.25f));
        nameField.setColour (juce::TextEditor::highlightedTextColourId, PresetPalette::ink());
        nameField.setColour (juce::CaretComponent::caretColourId, PresetPalette::blue());
        nameField.setIndents (12, 0);
        nameField.setJustification (juce::Justification::centredLeft);
        nameField.setInputRestrictions (48);
        nameField.setSelectAllWhenFocused (true);
        nameField.setTitle ("Preset name");
        nameField.setComponentID ("presetNameField");
        nameField.onTextChange = [this] { updateSaveButton(); };
        nameField.onReturnKey = [this] { save(); };
        nameField.onEscapeKey = [this] { close(); };
        addAndMakeVisible (nameField);

        cancelButton.setComponentID ("presetCancel");
        saveButton.setComponentID ("presetConfirm");

        for (auto* b : { &cancelButton, &saveButton })
        {
            b->setMouseCursor (juce::MouseCursor::PointingHandCursor);
            addAndMakeVisible (*b);
        }

        cancelButton.onClick = [this] { close(); };
        saveButton.onClick = [this] { save(); };
        setVisible (false);
    }

    ~SavePresetPanel() override                   { setLookAndFeel (nullptr); }

    void open (const juce::String& suggestedName)
    {
        message = {};
        nameField.setText (suggestedName, juce::dontSendNotification);
        updateSaveButton();
        setVisible (true);
        toFront (false);

        if (nameField.isShowing())
            nameField.grabKeyboardFocus();
        nameField.selectAll();
    }

    void close()
    {
        setVisible (false);
        giveAwayKeyboardFocus();   // rend le clavier au DAW (barre d'espace, etc.)
    }

    /** Pour les tests. */
    void typeName (const juce::String& text)      { nameField.setText (text, juce::sendNotification); }
    void confirm()                                { save(); }
    juce::String getSaveButtonText() const        { return saveButton.getButtonText(); }

    std::function<void()> onSaved;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! box.contains (e.getPosition()))
            close();
    }

    void resized() override
    {
        box = juce::Rectangle<int> (440, 214).withCentre (getLocalBounds().getCentre());
        nameField.setBounds (box.getX() + 28, box.getY() + 64, box.getWidth() - 56, 40);
        const int y = box.getBottom() - 26 - 36;
        saveButton.setBounds (box.getRight() - 28 - 110, y, 110, 36);
        cancelButton.setBounds (saveButton.getX() - 10 - 110, y, 110, 36);
    }

    void paint (juce::Graphics& g) override
    {
        using P = PresetPalette;
        g.fillAll (juce::Colour (0xff1b2b4b).withAlpha (0.38f));

        const auto shape = P::chamfered (box.toFloat(), 14.0f);
        juce::DropShadow (juce::Colour (0xff1b2b4b).withAlpha (0.45f), 18, { 0, 6 }).drawForPath (g, shape);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffcfdfe), 0.0f, (float) box.getY(),
                                                 juce::Colour (0xffeceff3), 0.0f, (float) box.getBottom(), false));
        g.fillPath (shape);
        g.setColour (juce::Colour (0xffa9b4c2));
        g.strokePath (shape, juce::PathStrokeType (1.3f));

        const auto inner = box.toFloat().reduced (28.0f, 0.0f);
        g.setColour (P::ink());
        g.setFont (lf.font (13.0f, true, 0.3f));
        g.drawText ("SAVE PRESET", inner.withY ((float) box.getY() + 24.0f).withHeight (20.0f), juce::Justification::centredLeft, false);

        g.setColour (message.isNotEmpty() ? juce::Colour (0xffd03a3a) : P::dimInk());
        g.setFont (lf.font (9.5f, true, 0.16f));
        const auto hint = juce::String (juce::CharPointer_UTF8 ("ENTER = SAVE \xc2\xb7 ESC = CANCEL"));
        g.drawText (message.isNotEmpty() ? message : hint, inner.withY ((float) box.getY() + 112.0f).withHeight (14.0f),
                    juce::Justification::centredLeft, false);
    }

private:
    void updateSaveButton()
    {
        const auto name = PresetLibrary::cleanName (nameField.getText());
        saveButton.setEnabled (name.isNotEmpty());
        saveButton.setButtonText (library.exists (name) ? "REPLACE" : "SAVE");
        message = {};
        repaint();
    }

    void save()
    {
        const auto name = PresetLibrary::cleanName (nameField.getText());
        if (name.isEmpty())
            return;

        if (! library.save (name))
        {
            message = "COULD NOT WRITE THE PRESET FILE";
            repaint();
            return;
        }

        close();
        if (onSaved)
            onSaved();
    }

    PresetLookAndFeel& lf;
    PresetLibrary& library;
    juce::TextEditor nameField;
    juce::TextButton cancelButton { "CANCEL" }, saveButton { "SAVE" };
    juce::String message;
    juce::Rectangle<int> box;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SavePresetPanel)
};

//==============================================================================
class PresetBar final : public juce::Component,
                        private juce::Timer
{
public:
    static constexpr int sizeChoices[] = { 60, 75, 100, 125, 150 };

    PresetBar (PresetLibrary& lib, FontMaker f)
        : library (lib), lf (std::move (f)), savePanel (lf, lib)
    {
        setLookAndFeel (&lf);

        prev.setComponentID ("presetPrev");
        name.setComponentID ("presetName");
        next.setComponentID ("presetNext");
        save.setComponentID ("presetSave");
        size.setComponentID ("windowSize");
        prev.setTitle ("Previous preset");
        next.setTitle ("Next preset");
        name.setTitle ("Presets");
        size.setTitle ("Window size");

        for (auto* b : { &prev, &name, &next, &save, &size })
        {
            b->setMouseCursor (juce::MouseCursor::PointingHandCursor);
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (*b);
        }

        prev.onClick = [this] { library.step (-1); refresh(); };
        next.onClick = [this] { library.step (+1); refresh(); };
        name.onClick = [this] { showPresetMenu(); };
        save.onClick = [this] { openSavePanel(); };
        size.onClick = [this] { showSizeMenu(); };
        savePanel.onSaved = [this] { refresh(); };

        refresh();
        startTimerHz (10);
    }

    ~PresetBar() override
    {
        stopTimer();
        setLookAndFeel (nullptr);
    }

    /** Le panneau SAVE couvre toute la façade : à ajouter à celle-ci. */
    void attachSavePanelTo (juce::Component& facade)
    {
        facade.addChildComponent (savePanel);
        savePanel.setBounds (facade.getLocalBounds());
    }

    SavePresetPanel& getSavePanel() noexcept       { return savePanel; }

    /** Taille de la fenêtre : lue et changée par l'éditeur (1 = 100 %). */
    std::function<float()> getScale;
    std::function<void (float)> setScale;

    /** Largeur idéale de la barre. */
    static constexpr int idealWidth = 26 + 4 + 172 + 4 + 26 + 8 + 56 + 8 + 60;

    void resized() override
    {
        const int h = getHeight(), arrow = 26, gap = 4;
        const int nameWidth = juce::jmax (90, getWidth() - (idealWidth - 172));
        int x = 0;
        prev.setBounds (x, 0, arrow, h);      x += arrow + gap;
        name.setBounds (x, 0, nameWidth, h);  x += nameWidth + gap;
        next.setBounds (x, 0, arrow, h);      x += arrow + 8;
        save.setBounds (x, 0, 56, h);         x += 56 + 8;
        size.setBounds (x, 0, 60, h);
    }

    void refresh()
    {
        shownName = library.getDisplayName() + (library.isModified() ? " *" : "");
        name.setButtonText (shownName);

        const int percent = getScale ? juce::roundToInt (getScale() * 100.0f) : 100;
        size.setButtonText (juce::String (percent) + " %");
    }

    void openSavePanel()
    {
        const auto& current = library.getCurrent();
        savePanel.open (current.user ? current.name : juce::String());
    }

private:
    void timerCallback() override
    {
        const int percent = getScale ? juce::roundToInt (getScale() * 100.0f) : 100;
        if (library.getDisplayName() + (library.isModified() ? " *" : "") != shownName
             || size.getButtonText() != juce::String (percent) + " %")
            refresh();
    }

    void showPresetMenu()
    {
        const auto list = library.getAllEntries();
        const int currentIndex = library.indexOfCurrent (list);
        const auto current = library.getCurrent();

        enum { saveAsId = 100001, deleteId, folderId, emptyId };

        juce::PopupMenu menu;
        menu.setLookAndFeel (&lf);
        menu.addSectionHeader ("FACTORY");
        bool userSection = false;

        for (size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].user && ! userSection)
            {
                menu.addSeparator();
                menu.addSectionHeader ("USER");
                userSection = true;
            }

            menu.addItem ((int) i + 1, list[i].name, true, (int) i == currentIndex);
        }

        if (! userSection)
        {
            menu.addSeparator();
            menu.addSectionHeader ("USER");
            menu.addItem (emptyId, "NO SAVED PRESET YET", false);
        }

        menu.addSeparator();
        menu.addItem (saveAsId, juce::String (juce::CharPointer_UTF8 ("SAVE AS\xe2\x80\xa6")));
        if (current.user && current.file.existsAsFile())
            menu.addItem (deleteId, "DELETE " + current.name.quoted());
        menu.addItem (folderId, "SHOW PRESETS FOLDER");

        juce::Component::SafePointer<PresetBar> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&name)
                                                      .withMinimumWidth (name.getWidth())
                                                      .withStandardItemHeight (24),
                            [safe, list] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;

                                auto& lib = safe->library;

                                if (result == saveAsId)
                                    safe->openSavePanel();
                                else if (result == deleteId)
                                    lib.remove (lib.getCurrent().file);
                                else if (result == folderId)
                                {
                                    const auto dir = lib.folder();
                                    dir.createDirectory();
                                    dir.startAsProcess();
                                }
                                else if (juce::isPositiveAndBelow (result - 1, (int) list.size()))
                                    lib.loadEntry (list[(size_t) (result - 1)]);

                                safe->refresh();
                            });
    }

    void showSizeMenu()
    {
        const int percent = getScale ? juce::roundToInt (getScale() * 100.0f) : 100;

        juce::PopupMenu menu;
        menu.setLookAndFeel (&lf);
        menu.addSectionHeader ("WINDOW SIZE");

        for (const int choice : sizeChoices)
            menu.addItem (choice, juce::String (choice) + " %", true, choice == percent);

        juce::Component::SafePointer<PresetBar> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&size).withStandardItemHeight (24),
                            [safe] (int result)
                            {
                                if (safe == nullptr || result == 0 || ! safe->setScale)
                                    return;

                                // L'éditeur peut changer de taille (et le menu disparaître) : on garde une copie.
                                auto apply = safe->setScale;
                                apply ((float) result / 100.0f);

                                if (safe != nullptr)
                                    safe->refresh();
                            });
    }

    PresetLibrary& library;
    PresetLookAndFeel lf;
    juce::TextButton prev, name, next, save { "SAVE" }, size { "100 %" };
    SavePresetPanel savePanel;
    juce::String shownName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};

} // namespace phragma
