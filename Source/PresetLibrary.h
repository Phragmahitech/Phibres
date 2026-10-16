#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    Presets : les points de départ d'usine + les presets de l'utilisateur (un fichier par preset).

    Dossier :
      macOS   ~/Library/Audio/Presets/Phragma/<Plugin>/
      Windows %APPDATA%\Phragma\<Plugin>\Presets\
    Un preset contient la valeur de tous les paramètres (en unités réelles : ms, Hz, %...).
    Le nom du preset en cours est gardé dans l'état du plugin (donc dans le projet du DAW).
*/
class PresetLibrary
{
public:
    /** Ce que le plugin sait de ses points de départ d'usine. */
    struct Factory
    {
        std::function<int()> count;
        std::function<juce::String (int)> name;
        std::function<void (int)> apply;
        /** Index du point de départ qui correspond aux réglages actuels, sinon -1. */
        std::function<int()> matching;
    };

    /** Un seul point de départ d'usine : toutes les valeurs par défaut (pour les effets sans presets d'usine). */
    static Factory defaultsOnly (juce::AudioProcessor& processor, juce::StringArray excluded, juce::String name = "DEFAULT")
    {
        auto stored = [excluded] (juce::AudioProcessorParameter* p)
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
            return ranged != nullptr && ! excluded.contains (ranged->getParameterID());
        };

        return { [] { return 1; },
                 [name] (int) { return name; },
                 [&processor, stored] (int)
                 {
                     for (auto* p : processor.getParameters())
                         if (stored (p))
                         {
                             p->beginChangeGesture();
                             p->setValueNotifyingHost (p->getDefaultValue());
                             p->endChangeGesture();
                         }
                 },
                 [&processor, stored]
                 {
                     for (auto* p : processor.getParameters())
                         if (stored (p) && std::abs (p->getValue() - p->getDefaultValue()) > 1.0e-4f)
                             return -1;
                     return 0;
                 } };
    }

    struct Entry
    {
        bool user = false;
        int factoryIndex = -1;
        juce::File file;
        juce::String name;
    };

    /** excluded : identifiants des paramètres qu'un preset ne garde pas (touches momentanées, etc.).
        emptyName : nom affiché quand aucun preset n'est choisi. */
    PresetLibrary (juce::AudioProcessorValueTreeState& s, juce::String plugin, juce::String extension, Factory f,
                   juce::StringArray excluded = {}, juce::String emptyName = "INIT")
        : apvts (s), pluginName (std::move (plugin)), fileExtension (std::move (extension)), factory (std::move (f)),
          excludedIds (std::move (excluded)), noPresetName (std::move (emptyName))
    {
    }

    /** Ce paramètre est-il gardé dans les presets ? */
    bool isStored (const juce::AudioProcessorParameter& p) const
    {
        if (auto* ranged = dynamic_cast<const juce::RangedAudioParameter*> (&p))
            return ! excludedIds.contains (ranged->getParameterID());
        return true;
    }

    //==========================================================================
    juce::File getFolder() const
    {
       #if JUCE_MAC
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                   .getChildFile ("Library/Audio/Presets/Phragma").getChildFile (pluginName);
       #else
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Phragma").getChildFile (pluginName).getChildFile ("Presets");
       #endif
    }

    /** Pour les tests : range les presets ailleurs. */
    void setFolderOverride (const juce::File& folder)   { folderOverride = folder; }

    juce::File folder() const                           { return folderOverride != juce::File() ? folderOverride : getFolder(); }

    /** Presets utilisateur, triés par nom (sans tenir compte des majuscules). */
    juce::Array<juce::File> getUserPresets() const
    {
        auto files = folder().findChildFiles (juce::File::findFiles, false, "*" + fileExtension);
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
        {
            return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0;
        });
        return files;
    }

    /** Usine puis utilisateur : l'ordre des flèches ‹ ›. */
    std::vector<Entry> getAllEntries() const
    {
        std::vector<Entry> list;

        for (int i = 0; i < factory.count(); ++i)
            list.push_back ({ false, i, {}, factory.name (i) });

        for (auto& f : getUserPresets())
            list.push_back ({ true, -1, f, f.getFileNameWithoutExtension() });

        return list;
    }

    //==========================================================================
    /** Nom de fichier propre (sans / : etc.), 48 caractères au plus. Vide si rien d'utilisable. */
    static juce::String cleanName (const juce::String& raw)
    {
        auto name = juce::File::createLegalFileName (raw.trim()).trim().substring (0, 48).trim();
        while (name.startsWithChar ('.'))
            name = name.substring (1).trim();
        return name;
    }

    juce::File fileFor (const juce::String& name) const   { return folder().getChildFile (cleanName (name) + fileExtension); }

    bool exists (const juce::String& name) const           { return cleanName (name).isNotEmpty() && fileFor (name).existsAsFile(); }

    /** Enregistre les réglages actuels sous ce nom (remplace un preset du même nom). */
    bool save (const juce::String& rawName)
    {
        const auto name = cleanName (rawName);
        if (name.isEmpty())
            return false;

        juce::XmlElement xml (pluginName + "Preset");
        xml.setAttribute ("version", 1);
        xml.setAttribute ("name", name);

        for (auto* p : apvts.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && isStored (*ranged))
            {
                auto* e = xml.createNewChildElement ("PARAM");
                e->setAttribute ("id", ranged->getParameterID());
                e->setAttribute ("value", (double) ranged->convertFrom0to1 (ranged->getValue()));
            }

        const auto dir = folder();
        if (! dir.createDirectory())
            return false;

        const auto file = fileFor (name);
        if (! xml.writeTo (file))
            return false;

        setCurrent ({ true, -1, file, name });
        return true;
    }

    /** Charge un fichier de preset. Les paramètres absents du fichier reviennent à leur valeur par défaut. */
    bool load (const juce::File& file)
    {
        const auto values = readFileValues (file);
        const auto& params = apvts.processor.getParameters();
        if (values.size() != (size_t) params.size())
            return false;

        for (int i = 0; i < params.size(); ++i)
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (params[i]); ranged != nullptr && isStored (*ranged))
                setParameter (*ranged, values[(size_t) i]);

        setCurrent ({ true, -1, file, file.getFileNameWithoutExtension() });
        return true;
    }

    void loadFactory (int index)
    {
        if (! juce::isPositiveAndBelow (index, factory.count()))
            return;

        factory.apply (index);
        setCurrent ({ false, index, {}, factory.name (index) });
    }

    bool loadEntry (const Entry& e)
    {
        if (e.user)
            return load (e.file);

        loadFactory (e.factoryIndex);
        return true;
    }

    /** Preset suivant (+1) ou précédent (-1), en boucle. */
    void step (int delta)
    {
        const auto list = getAllEntries();
        if (list.empty())
            return;

        int index = indexOfCurrent (list);
        if (index < 0)
            index = delta > 0 ? -1 : 0;

        const int n = (int) list.size();
        const int next = ((index + delta) % n + n) % n;
        loadEntry (list[(size_t) next]);
    }

    /** Envoie le preset à la corbeille (récupérable). */
    bool remove (const juce::File& file)
    {
        if (! file.existsAsFile() || ! file.moveToTrash())
            return false;

        if (current.user && current.file == file)
        {
            current.user = false;
            current.file = juce::File();
            current.name = {};
            writeCurrentToState();
        }

        return true;
    }

    //==========================================================================
    const Entry& getCurrent() const noexcept               { return current; }

    int indexOfCurrent (const std::vector<Entry>& list) const
    {
        for (size_t i = 0; i < list.size(); ++i)
        {
            const auto& e = list[i];
            if (e.user == current.user && (e.user ? e.file == current.file : e.factoryIndex == current.factoryIndex))
                return (int) i;
        }

        return -1;
    }

    /** Nom affiché : emptyName (« INIT » par défaut) si aucun preset choisi. */
    juce::String getDisplayName() const                    { return current.name.isNotEmpty() ? current.name : noPresetName; }

    /** Les réglages ont changé depuis le chargement / l'enregistrement du preset en cours. */
    bool isModified() const
    {
        if (current.name.isEmpty())
            return false;

        if (! current.user)
            return factory.matching() != current.factoryIndex;

        const auto& params = apvts.processor.getParameters();
        if (snapshot.size() != (size_t) params.size())
            return true;

        for (int i = 0; i < params.size(); ++i)
            if (isStored (*params[i]) && std::abs (params[i]->getValue() - snapshot[(size_t) i]) > 1.0e-4f)
                return true;

        return false;
    }

    //==========================================================================
    /** À appeler après apvts.replaceState (chargement d'un projet). */
    void restoreFromState()
    {
        const auto name = apvts.state.getProperty ("presetName").toString();
        const auto path = apvts.state.getProperty ("presetFile").toString();
        const int index = (int) apvts.state.getProperty ("presetFactory", -1);

        current = {};
        takeSnapshot();

        if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
        {
            current = { true, -1, juce::File (path), name };

            // Si le fichier est encore là, on compare aux valeurs du fichier (l'étoile reste juste)
            auto fromFile = readFileValues (current.file);
            if (! fromFile.empty())
                snapshot = std::move (fromFile);
        }
        else if (juce::isPositiveAndBelow (index, factory.count()))
        {
            current = { false, index, {}, factory.name (index) };
        }
        else if (name.isEmpty() && factory.matching() >= 0)
        {
            // Ancien projet sans nom de preset : on reconnaît un point de départ si possible
            current = { false, factory.matching(), {}, factory.name (factory.matching()) };
        }

        writeCurrentToState();
    }

    /** Pour l'interface : un point de départ choisi avec les boutons START FROM. */
    void markFactory (int index)                           { setCurrent ({ false, index, {}, factory.name (index) }); }

private:
    static void setParameter (juce::RangedAudioParameter& p, float normalised)
    {
        p.beginChangeGesture();
        p.setValueNotifyingHost (normalised);
        p.endChangeGesture();
    }

    void setCurrent (Entry e)
    {
        current = std::move (e);
        writeCurrentToState();
        takeSnapshot();
    }

    void writeCurrentToState()
    {
        apvts.state.setProperty ("presetName", current.name, nullptr);
        apvts.state.setProperty ("presetFile", current.user ? current.file.getFullPathName() : juce::String(), nullptr);
        apvts.state.setProperty ("presetFactory", current.user ? -1 : current.factoryIndex, nullptr);
    }

    /** Valeurs normalisées d'un fichier de preset, dans l'ordre des paramètres (vide si illisible). */
    std::vector<float> readFileValues (const juce::File& file) const
    {
        std::vector<float> values;
        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName (pluginName + "Preset"))
            return values;

        for (auto* p : apvts.processor.getParameters())
        {
            float normalised = p->getDefaultValue();

            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
                for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
                    if (e->getStringAttribute ("id") == ranged->getParameterID())
                    {
                        const auto value = (float) e->getDoubleAttribute ("value");
                        if (std::isfinite (value))
                        {
                            const auto& range = ranged->getNormalisableRange();
                            normalised = juce::jlimit (0.0f, 1.0f, ranged->convertTo0to1 (juce::jlimit (range.start, range.end, value)));
                        }
                    }

            values.push_back (normalised);
        }

        return values;
    }

    void takeSnapshot()
    {
        snapshot.clear();
        for (auto* p : apvts.processor.getParameters())
            snapshot.push_back (p->getValue());
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::String pluginName, fileExtension;
    Factory factory;
    juce::StringArray excludedIds;
    juce::String noPresetName;
    juce::File folderOverride;
    Entry current;
    std::vector<float> snapshot;
};
