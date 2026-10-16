#pragma once

// Tests communs : presets utilisateur et taille de la fenêtre.
// À inclure dans PluginTests.cpp après la fonction check() et les en-têtes du plugin.

template <typename Processor, typename Editor, typename Facade>
static void testPresetsAndWindowSize (const juce::StringArray& excluded, const juce::File& outputDir)
{
    std::cout << "- presets et taille de la fenetre" << std::endl;

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getNonexistentChildFile ("phragma-presets-test", "", false);

    std::unique_ptr<juce::AudioProcessor> base (createPluginFilter());
    auto& p = dynamic_cast<Processor&> (*base);
    auto& lib = p.presets;
    lib.setFolderOverride (folder);

    check (lib.getDisplayName() == "DEFAULT", "preset d'un plugin neuf : " + lib.getDisplayName());
    check (! lib.isModified(), "plugin neuf marque modifie");
    check (lib.getAllEntries().size() == 1, "il ne devrait y avoir que DEFAULT");

    // Paramètres gardés par les presets : on en change trois (continus de préférence)
    std::vector<juce::RangedAudioParameter*> stored, continuous;
    for (auto* param : p.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            check (lib.isStored (*ranged) == ! excluded.contains (ranged->getParameterID()), "exclusion : " + ranged->getParameterID());
            if (lib.isStored (*ranged))
            {
                stored.push_back (ranged);
                if (ranged->getNumSteps() > 100)
                    continuous.push_back (ranged);
            }
        }

    check (continuous.size() >= 3, "pas assez de parametres continus");
    if (continuous.size() < 3)
        return;

    auto set = [] (juce::RangedAudioParameter* param, float normalised) { param->setValueNotifyingHost (normalised); };
    auto near = [] (float a, float b) { return std::abs (a - b) < 2.0e-3f; };

    std::vector<float> chosen;
    for (size_t i = 0; i < 3; ++i)
    {
        const float target = continuous[i]->getDefaultValue() < 0.5f ? 0.73f + 0.05f * (float) i : 0.21f + 0.05f * (float) i;
        set (continuous[i], target);
        chosen.push_back (continuous[i]->getValue());
    }

    check (lib.isModified(), "reglages changes : etoile absente");

    // Enregistrement
    check (! lib.save ("  ../ "), "nom vide accepte");
    check (lib.save ("Acid / Rain: 01"), "enregistrement refuse");
    const auto file = lib.getCurrent().file;
    check (file.existsAsFile(), "fichier de preset absent");
    check (file.getParentDirectory() == folder, "preset hors du dossier");
    check (lib.getDisplayName() == file.getFileNameWithoutExtension(), "nom du preset en cours : " + lib.getDisplayName());
    check (! lib.isModified(), "preset juste enregistre marque modifie");
    check (lib.exists (lib.getDisplayName()), "exists() faux apres enregistrement");

    // Les paramètres exclus ne sont pas dans le fichier
    if (auto xml = juce::XmlDocument::parse (file))
    {
        int count = 0;
        for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
        {
            ++count;
            check (! excluded.contains (e->getStringAttribute ("id")), "parametre exclu enregistre : " + e->getStringAttribute ("id"));
        }
        check (count == (int) stored.size(), "nombre de parametres dans le preset : " + juce::String (count));
    }
    else
    {
        check (false, "preset illisible");
    }

    // Retour aux valeurs par défaut, puis rechargement du preset
    lib.loadFactory (0);
    check (lib.getDisplayName() == "DEFAULT" && ! lib.isModified(), "DEFAULT apres loadFactory");
    for (auto* param : stored)
        check (near (param->getValue(), param->getDefaultValue()), "DEFAULT ne remet pas " + param->getParameterID());

    check (lib.load (file), "chargement du preset");
    for (size_t i = 0; i < 3; ++i)
        check (near (continuous[i]->getValue(), chosen[i]), "valeur non rechargee : " + continuous[i]->getParameterID());
    check (! lib.isModified(), "preset charge marque modifie");

    // Flèches : DEFAULT <-> preset, en boucle
    lib.step (+1);
    check (lib.getDisplayName() == "DEFAULT", "fleche suivante (boucle)");
    lib.step (-1);
    check (lib.getCurrent().user && lib.getCurrent().file == file, "fleche precedente");

    // L'état du projet garde le nom du preset et l'étoile
    set (continuous[0], 0.5f);
    check (lib.isModified(), "modifie apres un reglage");
    {
        juce::MemoryBlock state;
        p.getStateInformation (state);

        std::unique_ptr<juce::AudioProcessor> other (createPluginFilter());
        auto& q = dynamic_cast<Processor&> (*other);
        q.presets.setFolderOverride (folder);
        q.setStateInformation (state.getData(), (int) state.getSize());
        check (q.presets.getCurrent().user && q.presets.getCurrent().file == file, "nom du preset non restaure avec le projet");
        check (q.presets.isModified(), "etoile non restauree avec le projet");
    }
    set (continuous[0], chosen[0]);
    check (! lib.isModified(), "retour aux valeurs du preset : etoile restee");

    // Fichiers abîmés : refusés sans rien changer
    const auto broken = folder.getChildFile ("broken" + file.getFileExtension());
    broken.replaceWithText ("<nope");
    check (! lib.load (broken), "fichier invalide accepte");
    check (lib.getCurrent().file == file, "preset en cours change par un fichier invalide");
    broken.deleteFile();

    // Valeur hors plage et paramètre absent
    {
        const auto partial = folder.getChildFile ("partial" + file.getFileExtension());
        juce::XmlElement xml (*juce::XmlDocument::parse (file));
        xml.deleteAllChildElements();
        auto* e = xml.createNewChildElement ("PARAM");
        e->setAttribute ("id", continuous[0]->getParameterID());
        e->setAttribute ("value", 1.0e9);
        xml.writeTo (partial);
        check (lib.load (partial), "preset partiel refuse");
        const float top = continuous[0]->convertTo0to1 (continuous[0]->getNormalisableRange().end);
        check (near (continuous[0]->getValue(), top), "valeur hors plage non bornee : " + continuous[0]->getParameterID() + " = " + juce::String (continuous[0]->getValue()));
        check (near (continuous[1]->getValue(), continuous[1]->getDefaultValue()), "parametre absent : pas de retour au defaut");
        partial.deleteFile();
    }

    //==========================================================================
    // Interface : barre des presets et taille de la fenêtre
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorAndMakeActive());
    auto* editor = dynamic_cast<Editor*> (ed.get());
    check (editor != nullptr, "pas d'editeur");
    if (editor == nullptr)
        return;

    auto& bar = editor->getFacade().getPresetBar();
    check (editor->getWidth() == Facade::width && editor->getHeight() == Facade::height, "taille par defaut");
    check (editor->isResizable(), "fenetre non redimensionnable");

    auto* nameButton = dynamic_cast<juce::TextButton*> (bar.findChildWithID ("presetName"));
    auto* sizeButton = dynamic_cast<juce::TextButton*> (bar.findChildWithID ("windowSize"));
    check (nameButton != nullptr && sizeButton != nullptr, "boutons de la barre absents");
    check (editor->getFacade().getLocalBounds().contains (editor->getFacade().getLocalArea (&bar, bar.getLocalBounds())), "barre hors de la facade");

    if (nameButton != nullptr && sizeButton != nullptr)
    {
        bar.refresh();
        check (nameButton->getButtonText() == lib.getDisplayName(), "nom affiche : " + nameButton->getButtonText());
        check (sizeButton->getButtonText() == "100 %", "taille affichee : " + sizeButton->getButtonText());

        set (continuous[2], continuous[2]->getValue() < 0.5f ? 0.9f : 0.1f);
        bar.refresh();
        check (nameButton->getButtonText().endsWith (" *"), "etoile absente de l'interface");
    }

    // Panneau SAVE : un nouveau nom puis REPLACE
    bar.openSavePanel();
    auto& panel = bar.getSavePanel();
    check (panel.isVisible(), "panneau SAVE invisible");
    check (editor->getFacade().getIndexOfChildComponent (&panel) >= 0, "panneau SAVE hors de la facade");

    if (outputDir != juce::File())
    {
        panel.typeName ("Space Drone");
        juce::PNGImageFormat png;
        juce::FileOutputStream out (outputDir.getChildFile ("ui_save_panel.png"));
        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            png.writeImageToStream (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), out);
        }
    }

    panel.typeName ("Space Drone");
    check (panel.getSaveButtonText() == "SAVE", "bouton SAVE pour un nom neuf");
    panel.confirm();
    check (! panel.isVisible(), "panneau resté ouvert apres SAVE");
    check (folder.getChildFile ("Space Drone" + file.getFileExtension()).existsAsFile(), "preset du panneau absent");
    check (lib.getDisplayName() == "Space Drone", "nom apres SAVE du panneau");

    bar.openSavePanel();
    check (panel.getSaveButtonText() == "REPLACE", "nom existant : REPLACE attendu");
    panel.close();
    check (lib.getAllEntries().size() == 3, "DEFAULT + 2 presets attendus");

    // Taille : 60 à 150 %, gardée avec le projet
    editor->setUiScale (1.5f);
    check (editor->getWidth() == juce::roundToInt (Facade::width * 1.5) && editor->getHeight() == juce::roundToInt (Facade::height * 1.5),
           "taille 150 % : " + juce::String (editor->getWidth()) + " x " + juce::String (editor->getHeight()));
    check (std::abs (editor->getFacade().getTransform().mat00 - 1.5f) < 0.01f, "facade non mise a l'echelle");
    bar.refresh();
    if (sizeButton != nullptr)
        check (sizeButton->getButtonText() == "150 %", "taille affichee a 150 % : " + sizeButton->getButtonText());

    editor->setUiScale (0.2f);
    check (editor->getWidth() == juce::roundToInt (Facade::width * 0.6), "taille non bornee a 60 %");

    if (outputDir != juce::File())
    {
        juce::PNGImageFormat png;
        juce::FileOutputStream out (outputDir.getChildFile ("ui_60_percent.png"));
        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            png.writeImageToStream (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), out);
        }
    }

    editor->setUiScale (1.25f);
    const int keptWidth = editor->getWidth();
    p.editorBeingDeleted (ed.get());
    ed.reset();

    {
        juce::MemoryBlock state;
        p.getStateInformation (state);
        std::unique_ptr<juce::AudioProcessor> other (createPluginFilter());
        other->setStateInformation (state.getData(), (int) state.getSize());
        std::unique_ptr<juce::AudioProcessorEditor> reopened (other->createEditorAndMakeActive());
        check (reopened != nullptr && reopened->getWidth() == keptWidth, "taille non gardee avec le projet");
        if (reopened != nullptr)
        {
            other->editorBeingDeleted (reopened.get());
            reopened.reset();
        }
    }

    // Suppression (corbeille)
    check (lib.remove (folder.getChildFile ("Space Drone" + file.getFileExtension())), "suppression refusee");
    check (! folder.getChildFile ("Space Drone" + file.getFileExtension()).existsAsFile(), "preset supprime encore present");
    check (lib.getDisplayName() == "UNTITLED", "nom apres suppression du preset en cours : " + lib.getDisplayName());

    folder.deleteRecursively();
}
