#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <optional>
#include <set>
#include <vector>
#include "GuiHelpers.h"
#include "AxesPanel.h"
#include "../presets/PresetFormat.h"
#include "../presets/TagVocabulary.h"

/**
 * Three-pane preset library overlay.
 *
 *   [ search…              ] [Get from GitHub] [Import] [Close]
 *   ┌──────────┬─────────────────────────┬─────────────┐
 *   │ Sidebar  │ Preset list             │ Detail card │
 *   │ Bank     │  name / prompt snippet  │ prompts /   │
 *   │ Model    │  modified date          │ axes / tags │
 *   │ Tags     │  …                      │ buttons     │
 *   └──────────┴─────────────────────────┴─────────────┘
 *   Status text
 *
 * The panel only describes selection; all I/O (load/save/delete) is
 * delegated to MainPanel via std::function callbacks. JSON-header parsing
 * (name / model / prompts / tags) happens during refreshLibrary() — light
 * enough to scan the whole library on each open.
 */
class PresetManagerPanel : public juce::Component,
                           public juce::DragAndDropContainer,
                           private juce::ListBoxModel,
                           private juce::KeyListener
{
public:
    struct Entry
    {
        juce::File   file;
        juce::String name;
        juce::String model;        // shortened display name (SA1 Open, SA3 Music, …)
        /** What the MODEL section of the sidebar files this preset under: the
         *  same label as `model` for a neural preset, "LRO" for one whose
         *  engine is the language oscillator. Separate from `model` because
         *  the two answer different questions — the Detail card's MODEL row
         *  reports what the FILE says (an LRO preset can still carry a neural
         *  id + audio buffer from before the bake, and that must not be
         *  overwritten), while this reports what actually SOUNDS. */
        juce::String sourceLabel;
        juce::String device;       // "mps"/"cuda"/"cpu" the audio was rendered on; empty = legacy
        juce::String engineMode;   // "Sampler" / "Wavetable" / "Granular"
        juce::String seqMode;      // "Off" / "Step" / "Gen" / "Step + Gen"
        juce::String promptA;
        juce::String promptB;
        /** The prompt that authored an LRO preset's orchestra
         *  (`engine.csound_prompt`). A separate field because it is a
         *  separate thing from the neural Impulse pair: promptA/B are the
         *  T5osc's prompt editors, and an LRO preset carries whatever text
         *  happened to be sitting in them when it was saved — for the
         *  library that is a caption written by a sound that isn't this
         *  one. Empty for an LRO preset saved before this field existed. */
        juce::String lroPrompt;
        juce::StringArray tags;
        juce::Time modified;
        /** Bank label: "My Presets" for files at the user-dir root, or the
         *  subdir name (joined with "/" if nested) for presets in user-created
         *  subdirectories like the GitHub-synced "UCDCAE AI Lab" bank. */
        juce::String bank;
        bool hasAxes = false;
        std::array<std::pair<juce::String, float>, 3> axes;
        double sampleRate = 0.0;
        int    numChannels = 0;
        int    numSamples  = 0;
        float  inferenceMs = 0.0f;   // 0 → unknown / legacy preset

        bool isLro() const { return engineMode == "LRO"; }

        /** One line of text that says what this preset was asked FOR — the
         *  list row's subtitle. Follows the engine that sounds: the LRO's
         *  authoring prompt for a language-oscillator preset, the neural
         *  Impulse pair for the rest. An LRO preset with no stored prompt
         *  returns empty and the row shows "—": the file genuinely does not
         *  know what was asked for, and captioning it with the Impulse text
         *  would put another sound's words under this one's name. */
        juce::String displayPrompt() const
        {
            if (isLro()) return lroPrompt;
            return promptA.isNotEmpty() ? promptA : promptB;
        }

        /** Everything the search box may match inside this preset's prompts.
         *  Same split, same reason: matching an LRO preset on its leftover
         *  Impulse text returns files that have nothing to do with the
         *  needle. */
        juce::String searchablePrompt() const
        {
            if (isLro()) return lroPrompt;
            return promptA + " " + promptB;
        }
    };

    /** Display label for the user-presets-root pseudo-bank. */
    static const juce::String& kRootUserBank()
    {
        static const juce::String s = "My Presets";
        return s;
    }

    enum class Mode { Browse, Save };

    /** Pre-fill payload for entering Save mode (name + bank + conflict UI).
     *  promptA/B feed the Save-Drawer's auto-title heuristic when no
     *  meaningful default name is available.
     *
     *  Note there is deliberately NO tag prefill: the drawer's chips
     *  follow the save TARGET (refreshSaveDrawerAutoTags) — overwriting
     *  an existing preset mirrors that file's on-disk tags, a fresh name
     *  starts untagged. */
    struct SavePrefill
    {
        juce::String           defaultName;
        juce::String           currentBank;
        juce::StringArray      existingBanks;
        std::set<juce::String> existingPathKeys;   // lowercased "bank/name.t5p"
        juce::String           promptA;
        juce::String           promptB;
        bool                   canIncludeInferenceCache = false;
    };

    PresetManagerPanel()
    {
        titleLabel.setText("Preset Library", juce::dontSendNotification);
        titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        titleLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        addAndMakeVisible(titleLabel);

        configureEditor(searchEditor, "Search name or prompt...");
        searchEditor.setEscapeAndReturnKeysConsumed(false);
        searchEditor.addKeyListener(this);
        searchEditor.onTextChange = [this] { rebuildFiltered(); };
        addAndMakeVisible(searchEditor);

        configureButton(importBtn, kSurface);
        importBtn.onClick = [this] { if (onImportRequested) onImportRequested(); };
        addAndMakeVisible(importBtn);

        configureButton(githubBtn, kSurface);
        githubBtn.onClick = [this] { if (onGetFromGithubRequested) onGetFromGithubRequested(); };
        addAndMakeVisible(githubBtn);

        // Top-right × icon — replaces the wider "Close" text button
        closeIconBtn.setButtonText(juce::String::fromUTF8("\xc3\x97"));
        closeIconBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        closeIconBtn.setColour(juce::TextButton::textColourOffId, kDim);
        closeIconBtn.setColour(juce::TextButton::textColourOnId,  juce::Colours::white);
        closeIconBtn.onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible(closeIconBtn);

        // Bottom-right Cancel button — explicit dismiss alongside the × icon
        // and the Esc keystroke.
        configureButton(cancelBtn, kSurface);
        cancelBtn.onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible(cancelBtn);

        setWantsKeyboardFocus(true);
        addKeyListener(this);

        sidebar.onChanged = [this]
        {
            rebuildFiltered();
            // In Save mode, clicking an explicit bank also targets the
            // drawer's Save destination. Toggling a bank off ("All") leaves
            // the drawer bank where the user last set it — the filter and
            // the save target are decoupled in that direction.
            if (currentMode == Mode::Save
                && sidebar.activeBank.isNotEmpty())
                saveDrawer.setBank(sidebar.activeBank);
        };
        sidebar.onCreateBankRequested = [this]
        {
            // Bank creation is a standalone disk action — not "the prelude
            // to a Save". Earlier this handler put focus into the
            // SaveDrawer's bank field and only materialised the directory
            // when the user committed a save, which entangled the two
            // concerns: a same-named save would then bounce off the
            // cross-bank uniqueness gate and the user would see nothing
            // happen and no folder created. Now: prompt for a name, mkdir
            // immediately, drop a `.t5ynth-bank` marker so the empty bank
            // is visible in the sidebar before any preset lives in it,
            // and only THEN — if Save mode is open — make the new bank
            // the current save target.
            auto* aw = new juce::AlertWindow(
                "New Bank",
                "Bank name (a folder under your presets directory):",
                juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor("name", "", {});
            aw->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
            aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            juce::Component::SafePointer<PresetManagerPanel> safeThis(this);
            aw->enterModalState(true,
                juce::ModalCallbackFunction::create(
                    [safeThis, aw](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> owner(aw);
                        if (safeThis == nullptr || result != 1) return;
                        auto* self = safeThis.getComponent();

                        const auto name = owner->getTextEditorContents("name").trim();
                        if (name.isEmpty()) return;
                        // Reject names that would collide with the
                        // root-pseudo-bank label, contain path
                        // separators, or look like a hidden directory.
                        if (name.startsWithChar('.')
                            || name.containsAnyOf("/\\")
                            || name.equalsIgnoreCase(PresetManagerPanel::kRootUserBank()))
                        {
                            juce::AlertWindow::showAsync(
                                juce::MessageBoxOptions()
                                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                                    .withTitle("Invalid Bank Name")
                                    .withMessage("Bank names cannot start with '.', "
                                                 "contain '/' or '\\', or use the "
                                                 "reserved label \""
                                                 + PresetManagerPanel::kRootUserBank()
                                                 + "\".")
                                    .withButton("OK")
                                    .withParentComponent(self),
                                nullptr);
                            return;
                        }

                        auto dir = PresetFormat::getUserPresetsDirectory()
                                       .getChildFile(name);
                        if (! dir.createDirectory().wasOk())
                        {
                            self->setStatusText("Could not create bank \""
                                                + name + "\"", true);
                            return;
                        }
                        // Marker file: lets refreshSidebarVocabulary
                        // surface an empty bank distinctly from sparse-
                        // checkout dirs like `scripts/` (no marker → not
                        // a bank). Idempotent: a re-created marker on an
                        // existing bank is harmless.
                        dir.getChildFile(".t5ynth-bank").create();

                        self->refreshLibrary();
                        self->sidebar.activeBank = name;
                        self->rebuildFiltered();
                        if (self->currentMode == Mode::Save)
                            self->saveDrawer.setBank(name);
                        self->repaint();
                    }));
        };
        addAndMakeVisible(sidebar);

        presetList.setModel(this);
        presetList.setColour(juce::ListBox::backgroundColourId, kSurface);
        presetList.setColour(juce::ListBox::outlineColourId, kBorder);
        presetList.setRowHeight(54);
        presetList.addKeyListener(this);
        addAndMakeVisible(presetList);

        detail.onLoadRequested = [this]
        {
            if (currentMode == Mode::Save) return;
            if (selectedEntryIndex < 0 || onLoadRequested == nullptr) return;
            onLoadRequested(allEntries[(size_t) selectedEntryIndex].file);
        };
        detail.onTagsCommitted = [this](const juce::StringArray& newTags)
        {
            if (selectedEntryIndex < 0 || onTagsChanged == nullptr) return;
            const auto& e = allEntries[(size_t) selectedEntryIndex];
            onTagsChanged(e.file, newTags);
        };
        detail.installEscListener(this);
        addAndMakeVisible(detail);

        saveDrawer.setVisible(false);
        saveDrawer.onSaveClicked = [this]
        {
            if (! onSaveRequested) return;
            const auto name = saveDrawer.getName();
            if (name.isEmpty()) return;
            onSaveRequested(name, saveDrawer.getTags(), saveDrawer.getBank(),
                            saveDrawer.getIncludeInferenceCache());
        };
        // Cancel from the Save-Drawer dismisses the entire overlay rather
        // than dropping the user into Browse mode — the user came for a
        // save action, "Cancel" means abort all the way back to the synth.
        saveDrawer.onCancelClicked = [this] { if (onCloseRequested) onCloseRequested(); };
        saveDrawer.onConflictChanged = [this]
        {
            computeConflictRow();
            refreshSaveDrawerAutoTags();
            presetList.repaint();
        };
        addChildComponent(saveDrawer);

        statusLabel.setColour(juce::Label::textColourId, kDim);
        statusLabel.setJustificationType(juce::Justification::centredLeft);
        statusLabel.setFont(juce::FontOptions(11.5f));
        addAndMakeVisible(statusLabel);
    }

    void paint(juce::Graphics& g) override
    {
        paintCard(g, getLocalBounds());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);

        // Top row: title + search + GitHub + Import + × close icon. The
        // GitHub/Import buttons are hidden in Save mode (they would be
        // confusing distractors while committing a new name) and so they do
        // not consume layout space.
        auto topRow = area.removeFromTop(28);
        auto titleArea = topRow.removeFromLeft(150);
        titleLabel.setBounds(titleArea.withTrimmedTop(4));
        closeIconBtn.setBounds(topRow.removeFromRight(28));
        topRow.removeFromRight(8);
        if (currentMode == Mode::Browse)
        {
            importBtn.setBounds(topRow.removeFromRight(112));
            topRow.removeFromRight(8);
            githubBtn.setBounds(topRow.removeFromRight(128));
            topRow.removeFromRight(8);
        }
        searchEditor.setBounds(topRow);

        area.removeFromTop(10);

        // Bottom area is mode-dependent: Browse keeps the slim status/cancel
        // footer; Save replaces it with the Save-Drawer (name/bank/tags/save).
        if (currentMode == Mode::Save)
        {
            // Sized so that after the drawer's own reduced(12, 8) padding
            // and the fixed name/warning/bank/tags/strip rows, the active
            // tag-chip area gets ~40 px and the vocabulary picker beneath
            // it gets ~70 px (good for two rows of suggestion chips).
            const int drawerH = 280;
            saveDrawer.setBounds(area.removeFromBottom(drawerH));
            area.removeFromBottom(6);
            // Save-mode status row: a thin strip below the panel chrome
            // (above the drawer) where the cross-bank uniqueness gate
            // and other setStatusText(..., true) calls become visible.
            // Without this, the label would render into zero-sized
            // bounds and the user wouldn't see the error message that
            // explains why their save was blocked.
            statusLabel.setBounds(area.removeFromBottom(22));
            area.removeFromBottom(4);
        }
        else
        {
            auto footer = area.removeFromBottom(28);
            cancelBtn.setBounds(footer.removeFromRight(96));
            footer.removeFromRight(8);
            statusLabel.setBounds(footer);
            area.removeFromBottom(6);
        }

        // Three panes — stay visible in both modes so the user can see the
        // existing library while typing a new name.
        sidebar.setBounds(area.removeFromLeft(140));
        area.removeFromLeft(8);
        detail.setBounds(area.removeFromRight(290));
        area.removeFromRight(8);
        presetList.setBounds(area);
    }

    void refreshLibrary()
    {
        // selectedEntryIndex points into allEntries, which is about to be
        // rebuilt and re-sorted — remember the FILE and re-resolve the
        // index afterwards. Without this, the restore path in
        // rebuildFiltered() would re-select whatever entry happens to sit
        // at the stale index after the sort.
        const juce::File prevSelectedFile =
            juce::isPositiveAndBelow(selectedEntryIndex, (int) allEntries.size())
                ? allEntries[(size_t) selectedEntryIndex].file
                : juce::File();

        allEntries.clear();

        for (auto& f : PresetFormat::getAllPresetFiles())
            allEntries.push_back(parseEntry(f));

        std::stable_sort(allEntries.begin(), allEntries.end(),
            [](const Entry& a, const Entry& b)
            {
                return a.name.compareIgnoreCase(b.name) < 0;
            });

        selectedEntryIndex = prevSelectedFile.getFullPathName().isNotEmpty()
                                 ? findEntryIndexForFile(prevSelectedFile)
                                 : -1;

        refreshSidebarVocabulary();

        rebuildFiltered();
        // Keep the Save-Drawer's cached conflict row in sync if the library
        // was rebuilt while in Save mode (the stable_sort above may have
        // moved entries, so the cached int could now point at the wrong row).
        if (currentMode == Mode::Save)
        {
            computeConflictRow();
            refreshSaveDrawerAutoTags();
        }
        setStatusText(allEntries.empty() ? "No presets found" : "");
    }

    void updateTagsForFile(const juce::File& file, const juce::StringArray& tags)
    {
        const int updatedIndex = findEntryIndexForFile(file);
        if (updatedIndex < 0) return;

        allEntries[(size_t) updatedIndex].tags = normaliseTags(tags);
        refreshSidebarVocabulary();
        rebuildFiltered();

        for (size_t row = 0; row < filteredIndices.size(); ++row)
        {
            if (filteredIndices[row] == updatedIndex)
            {
                selectRowAndSyncDetail((int) row);
                presetList.repaint();
                return;
            }
        }

        if (selectedEntryIndex == updatedIndex)
        {
            selectedEntryIndex = -1;
            detail.clear();
        }
        presetList.repaint();
    }

    /** Push this machine's backend device so the Detail card can flag presets
     *  rendered on a different device type (their audio won't reproduce here;
     *  prompts and synth settings still transfer). */
    void setRuntimeDevice(const juce::String& dev) { detail.setRuntimeDevice(dev); }

    void setCurrentPreset(const juce::File& file, const juce::String& name)
    {
        currentFile = file;
        currentName = name;
        // Try to highlight the matching row
        for (size_t i = 0; i < filteredIndices.size(); ++i)
        {
            const auto& e = allEntries[(size_t) filteredIndices[i]];
            if (e.file == file)
            {
                selectRowAndSyncDetail((int) i);
                return;
            }
        }
        // No match: leave previous selection
    }

    /** Select a row in the preset list by file, WITHOUT updating
     *  currentFile/currentName (the "currently loaded" badge). Used by
     *  the Phase-4 fork flow so the user sees their fresh fork in the
     *  list while the loaded-preset indicator stays on whatever they
     *  actually have loaded in the synth. */
    void selectFileInList(const juce::File& file)
    {
        for (size_t i = 0; i < filteredIndices.size(); ++i)
        {
            const auto& e = allEntries[(size_t) filteredIndices[i]];
            if (e.file == file)
            {
                selectRowAndSyncDetail((int) i);
                return;
            }
        }
    }

    void setStatusText(const juce::String& s, bool isError = false)
    {
        statusLabel.setText(s, juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId,
                              isError ? kErrorText : kDim);
    }

    /** Disable the Update / Import buttons while a background sync is in
     *  progress and surface the latest message in the status line. Caller
     *  (MainPanel + PresetUpdater) is responsible for clearing the busy
     *  state once the sync finishes. */
    void setUpdaterBusy(bool busy, const juce::String& message = {})
    {
        updaterBusy = busy;
        githubBtn.setEnabled(! busy);
        importBtn.setEnabled(! busy);
        if (message.isNotEmpty())
            setStatusText(message);
    }

    Mode getMode() const { return currentMode; }

    /** Owner-side push of the curated tag taxonomy. The panel merges this
     *  with the sidebar-derived user-tag set and propagates the union to
     *  Detail (browse-mode cloud) and SaveDrawer (save-mode cloud). Safe
     *  to call repeatedly; the conditional "generative" entry tracks the
     *  GenSeq parameter state at the call-site. */
    void setTagVocabularyCanonical(const juce::StringArray& canonical)
    {
        canonicalTagVocabulary = canonical;
        applyTagVocabulary();
    }

    /** Enter Save mode: hides the regular footer (status + Cancel) and
     *  shows the Save-Drawer at the bottom of the panel. The 3-pane
     *  Sidebar / Preset-list / Detail layout stays visible above so the
     *  user sees the existing library while typing the new name. */
    void enterSaveMode(SavePrefill prefill)
    {
        currentMode = Mode::Save;
        titleLabel.setText("Save Preset", juce::dontSendNotification);
        importBtn.setVisible(false);
        githubBtn.setVisible(false);
        cancelBtn.setVisible(false);
        // Keep statusLabel visible in Save mode so cross-bank uniqueness
        // failures (or any other setStatusText(..., true) call from the
        // owner) actually surface — the AlertWindow popup is easy to miss
        // when focus is on the SaveDrawer text field. Clear any stale
        // text first so the user sees fresh status only.
        setStatusText({});
        statusLabel.setVisible(true);
        sidebar.setSaveMode(true);
        rebuildFiltered();
        // Cloud vocab = user/library tags first, canonical taxonomy after.
        // Falls back to the sidebar set if MainPanel hasn't pushed a
        // canonical taxonomy yet (e.g. unit-test embedding of the panel).
        const auto& vocab = mergedTagVocabulary.empty()
                                ? sidebar.getTagVocabulary()
                                : mergedTagVocabulary;
        // configure() ends in refreshConflictUi() → onConflictChanged →
        // computeConflictRow() + refreshSaveDrawerAutoTags(), so the tag
        // chips are auto-filled from the initial save target right here.
        saveDrawer.configure(prefill.defaultName,
                             prefill.currentBank,
                             prefill.existingBanks,
                             std::move(prefill.existingPathKeys),
                             vocab,
                             prefill.promptA,
                             prefill.promptB,
                             prefill.canIncludeInferenceCache);
        saveDrawer.setVisible(true);
        detail.setBrowseActionsVisible(false);
        detail.setDragSourceEnabled(true);
        resized();
        repaint();
        saveDrawer.focusName();
    }

    /** Restore the regular Browse layout. Idempotent. */
    void leaveSaveMode()
    {
        if (currentMode == Mode::Browse) return;
        currentMode = Mode::Browse;
        titleLabel.setText("Preset Library", juce::dontSendNotification);
        importBtn.setVisible(true);
        githubBtn.setVisible(true);
        cancelBtn.setVisible(true);
        statusLabel.setVisible(true);
        saveDrawer.setVisible(false);
        sidebar.setSaveMode(false);
        rebuildFiltered();
        detail.setBrowseActionsVisible(true);
        detail.setDragSourceEnabled(false);
        conflictEntryIndex = -1;
        resized();
        repaint();
    }

    // Owner-side callbacks: load / save / import / delete / rename / duplicate / reveal / tags / close.
    std::function<void(const juce::File&)>                              onLoadRequested;
    std::function<void(const juce::File&)>                              onDeleteRequested;
    std::function<void(const juce::File&)>                              onRenameRequested;
    std::function<void(const juce::File&)>                              onDuplicateRequested;
    std::function<void(const juce::File&)>                              onRevealRequested;
    std::function<void()>                                                onImportRequested;
    std::function<void()>                                                onGetFromGithubRequested;
    std::function<void()>                                                onCloseRequested;
    std::function<void(const juce::File&, const juce::StringArray&)>    onTagsChanged;
    std::function<void(const juce::String& name,
                       const juce::StringArray& tags,
                       const juce::String& bank,
                       bool includeInferenceCache)>                      onSaveRequested;

private:
    // ─── Helpers ─────────────────────────────────────────────────────────

    /** Visual variants of the tag-chip primitive shared by Detail-card,
     *  Save-Drawer active set, and Save-Drawer suggestion cloud. */
    enum class ChipKind
    {
        ActiveRemovable,   // Detail-card + Save-Drawer active set: filled, with × hitbox
        Suggestion         // Save-Drawer cloud: outline-only, click to add
    };

    /** Render one tag-chip. Returns the bounding rectangle so callers can
     *  cache it for hit-testing. Geometry (rowH, padding, font, corner
     *  radius) is identical across variants — only fill style and the
     *  trailing × differ — so chips read as the same primitive wherever
     *  they appear. */
    static juce::Rectangle<int> paintTagChip(juce::Graphics& g,
                                             int x, int y,
                                             const juce::String& label,
                                             ChipKind kind)
    {
        const int rowH = 22;
        const int chipH = rowH - 2;
        const int paddingX = (kind == ChipKind::ActiveRemovable) ? 28 : 14;
        const int chipW = juce::Font(juce::FontOptions(11.0f)).getStringWidth(label) + paddingX;
        const juce::Rectangle<int> chip(x, y, chipW, chipH);

        if (kind == ChipKind::Suggestion)
        {
            g.setColour(kBorder);
            g.drawRoundedRectangle(chip.toFloat(), 3.0f, 1.0f);
        }
        else
        {
            g.setColour(kSurface.brighter(0.08f));
            g.fillRoundedRectangle(chip.toFloat(), 3.0f);
            g.setColour(kBorder);
            g.drawRoundedRectangle(chip.toFloat(), 3.0f, 1.0f);
        }

        g.setColour(kind == ChipKind::Suggestion ? juce::Colours::white.withAlpha(0.78f)
                                                 : juce::Colours::white);
        g.setFont(juce::FontOptions(11.0f));
        auto labelArea = chip.reduced(7, 2);
        if (kind == ChipKind::ActiveRemovable) labelArea.removeFromRight(14);
        g.drawText(label, labelArea, juce::Justification::centredLeft, false);

        if (kind == ChipKind::ActiveRemovable)
        {
            const juce::Rectangle<int> closeBox(chip.getRight() - 16, chip.getY(),
                                                16, chip.getHeight());
            g.setColour(kDimmer);
            g.drawText(juce::String::fromUTF8("\xc3\x97"), closeBox,
                       juce::Justification::centred, false);
        }
        return chip;
    }

    /** The "Known tags - click to add" cloud, shared by the Detail card and
     *  the Save-Drawer.
     *
     *  It is a Viewport over a chip flow, and that is the whole point: both
     *  hosts previously painted the chips straight into a fixed one-to-three
     *  row box and `break`-ed out of the loop as soon as the next row did not
     *  fit. Whatever came after was gone — no scrollbar, no "+N more", no
     *  indication at all — so which tags the user could click was decided by
     *  the vocabulary's ORDER against a pixel budget. That turned the merge
     *  order in applyTagVocabulary() into a load-bearing UI decision and made
     *  every tag past the fold unreachable except by typing it exactly.
     *
     *  Here the flow lays out every tag it is given and reports its true
     *  height; the Viewport scrolls. Nothing is dropped, so no caller has to
     *  reason about what fits. */
    class TagCloud : public juce::Component
    {
    public:
        TagCloud()
        {
            flow.owner = this;
            viewport.setViewedComponent(&flow, false);
            viewport.setScrollBarsShown(true, false);
            viewport.setScrollBarThickness(kScrollBarW);
            addAndMakeVisible(viewport);
        }

        /** Fired with the clicked tag. The host owns what "add" means. */
        std::function<void(const juce::String&)> onTagPicked;

        /** The full vocabulary, in the order the owner merged it. Order is
         *  now cosmetic — every entry is reachable either way. */
        void setVocabulary(std::vector<juce::String> v)
        {
            vocabulary = std::move(v);
            rebuild();
        }
        bool hasVocabulary() const noexcept { return ! vocabulary.empty(); }

        /** `excluded` = the tags already on the preset (never offered twice);
         *  `filterText` = the tag input's prefix autocomplete. */
        void update(const juce::StringArray& excluded, const juce::String& filterText)
        {
            excludedTags = excluded;
            filter = filterText.trim();
            rebuild();
        }

        void paint(juce::Graphics& g) override
        {
            g.setColour(kDim);
            g.setFont(juce::FontOptions(kUiLabelFontMin, juce::Font::bold));
            g.drawText("Known tags - click to add",
                       getLocalBounds().removeFromTop(kHeaderH),
                       juce::Justification::centredLeft, false);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            r.removeFromTop(kHeaderH + 2);
            viewport.setBounds(r);
            rebuild();
        }

        /** Header + one chip row: below this the component can show nothing
         *  at all and the host should not reserve space for it. */
        static constexpr int minimumUsefulHeight() { return kHeaderH + 2 + kRowH; }

        /** Header + n chip rows, exactly. Hosts used to cap this cloud at a flat
         *  100 px, which is two rows and a sliver of a third whatever the drawer
         *  was actually offering - the chips scroll, so the cap read as "there
         *  are only two rows of tags" rather than "there is no more room". */
        static constexpr int heightForRows(int n)
        { return kHeaderH + 2 + n * kRowH + (n - 1) * kGapY; }

    private:
        static constexpr int kHeaderH = 14;
        static constexpr int kRowH = 22;
        static constexpr int kGapX = 4;
        static constexpr int kGapY = 4;
        static constexpr int kScrollBarW = 8;

        /** The scrolled content: all matching chips, laid out and hit-tested
         *  in its own coordinate space so the Viewport's scroll offset needs
         *  no correction anywhere. */
        struct Flow : public juce::Component
        {
            TagCloud* owner = nullptr;
            std::vector<std::pair<juce::Rectangle<int>, juce::String>> chips;

            void paint(juce::Graphics& g) override
            {
                for (const auto& c : chips)
                    PresetManagerPanel::paintTagChip(g, c.first.getX(), c.first.getY(),
                                                     c.second, ChipKind::Suggestion);
            }

            void mouseUp(const juce::MouseEvent& e) override
            {
                // A drag inside the viewport is a scroll gesture, not a pick.
                if (e.mouseWasDraggedSinceMouseDown()) return;
                const auto p = e.getPosition();
                for (const auto& c : chips)
                    if (c.first.contains(p))
                    {
                        // COPY before calling out: the host's handler adds the
                        // tag and then refreshes the cloud, which clears this
                        // very vector — a reference into it would dangle mid-
                        // callback.
                        const auto picked = c.second;
                        if (owner != nullptr && owner->onTagPicked)
                            owner->onTagPicked(picked);
                        return;
                    }
            }
        };

        void rebuild()
        {
            // The scrollbar's width is reserved whether or not it is showing,
            // so the flow's width — and with it the row breaks and the total
            // height — never depends on the scrollbar that height decides.
            const int w = juce::jmax(20, viewport.getWidth() - kScrollBarW);
            flow.chips.clear();
            int x = 0, y = 0;
            for (const auto& t : vocabulary)
            {
                if (t.isEmpty() || excludedTags.contains(t, true)) continue;
                if (filter.isNotEmpty() && ! t.startsWithIgnoreCase(filter)) continue;
                const int chipW = juce::Font(juce::FontOptions(11.0f)).getStringWidth(t) + 14;
                // x > 0 keeps a chip wider than the whole row on its own row
                // instead of pushing an empty one ahead of it.
                if (x > 0 && x + chipW > w)
                {
                    x = 0;
                    y += kRowH + kGapY;
                }
                flow.chips.push_back({ { x, y, chipW, kRowH - 2 }, t });
                x += chipW + kGapX;
            }
            flow.setSize(w, flow.chips.empty() ? 0 : y + kRowH);
            flow.repaint();
        }

        // Declared before the Viewport so the Viewport is destroyed first and
        // can never observe a half-destroyed viewed component.
        Flow flow;
        juce::Viewport viewport;
        std::vector<juce::String> vocabulary;
        juce::StringArray excludedTags;
        juce::String filter;
    };

    static void configureEditor(juce::TextEditor& e, const juce::String& placeholder)
    {
        e.setTextToShowWhenEmpty(placeholder, kDimmer);
        e.setColour(juce::TextEditor::backgroundColourId, kSurface.brighter(0.04f));
        e.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        e.setColour(juce::TextEditor::outlineColourId, kBorder);
        e.setColour(juce::TextEditor::focusedOutlineColourId, kAccent);
        e.setSelectAllWhenFocused(true);
    }
    static void configureButton(juce::TextButton& b, juce::Colour c)
    {
        b.setColour(juce::TextButton::buttonColourId, c);
        b.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    }

    static juce::String snippet(const juce::String& s, int maxLen = 84)
    {
        auto t = s.trim();
        if (t.length() <= maxLen) return t;
        return t.substring(0, maxLen - 1).trimEnd()
             + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"));
    }

    /** Bank label derived from a preset's location on disk:
     *    user dir root      → "My Presets"
     *    user dir subdir(s) → joined subdirectory path, e.g. "Ambient" or
     *                         "Drones/Long" or "UCDCAE AI Lab".
     *  This is the ONLY place that maps disk layout to bank semantics so
     *  the same logic can be reused by Save (where target dir is derived
     *  from a chosen bank name). */
    static juce::String computeBankLabel(const juce::File& file)
    {
        const auto userRoot = PresetFormat::getUserPresetsDirectory();
        auto parent = file.getParentDirectory();
        if (parent == userRoot) return kRootUserBank();
        const auto rel = parent.getRelativePathFrom(userRoot);
        return rel.isEmpty() ? kRootUserBank() : rel.replace("\\", "/");
    }

    /** Map full HuggingFace model IDs to short labels used in the GUI
     *  (matches PromptPanel's switchbox labels). Unknown IDs come back
     *  unchanged so future models still display, just longer.
     *
     *  Order is significant: "stable-audio-open-small" must precede
     *  "stable-audio-open" because the latter prefix matches the former. */
    static juce::String shortenModelName(const juce::String& full)
    {
        const auto lc = full.toLowerCase();
        if (lc.contains("stable-audio-3"))          return lc.contains("sfx") ? "SA3 SFX" : "SA3 Music";
        if (lc.contains("stable-audio-open-small")) return "SA1 Small";
        if (lc.contains("stable-audio-open"))       return "SA1 Open";
        if (lc.contains("audioldm"))                return "AudioLDM2";
        return full;
    }

    static juce::StringArray normaliseTags(const juce::StringArray& input)
    {
        juce::StringArray tags = input;
        tags.trim();
        tags.removeEmptyStrings();
        tags.removeDuplicates(true);
        return tags;
    }

    int findEntryIndexForFile(const juce::File& file) const
    {
        for (size_t i = 0; i < allEntries.size(); ++i)
            if (allEntries[i].file == file)
                return (int) i;

        return -1;
    }

    /** Programmatic row selection + explicit Detail-card sync.
     *
     *  JUCE's ListBox::selectRow() only fires selectedRowsChanged when the
     *  selected row NUMBER actually changes (selectRowInternal early-outs
     *  on an already-selected row). After a refilter the same row number
     *  frequently maps to a DIFFERENT entry — e.g. row 0 stays selected
     *  while a sidebar tag click swaps the list contents — and the Detail
     *  card would silently keep showing the previous preset. Every
     *  programmatic selection therefore goes through this helper, which
     *  pushes the entry into the Detail card unconditionally instead of
     *  relying on the callback. */
    void selectRowAndSyncDetail(int row)
    {
        if (! juce::isPositiveAndBelow(row, (int) filteredIndices.size())) return;
        juce::ScopedValueSetter<bool> sv(programmaticSelection, true);
        presetList.selectRow(row, false, true);
        selectedEntryIndex = filteredIndices[(size_t) row];
        detail.setEntry(allEntries[(size_t) selectedEntryIndex]);
    }

    void refreshSidebarVocabulary()
    {
        // Sidebar vocabulary — banks come from actual disk layout, models and
        // tags from saved preset metadata. Tags remain user-editable state; the
        // Save dialog is where heuristic suggestions are proposed.
        std::set<juce::String> banks, models, tags;
        for (auto& e : allEntries)
        {
            if (e.bank.isNotEmpty())  banks.insert(e.bank);
            if (e.sourceLabel.isNotEmpty()) models.insert(e.sourceLabel);
            for (auto& t : e.tags)    tags.insert(t);
        }

        // Also surface EMPTY banks created via the "+" button. A directory
        // counts as a bank iff it carries the `.t5ynth-bank` marker that
        // we drop in onCreateBankRequested. This filter is essential
        // because the user-presets root can contain non-bank dirs from
        // the public mirror's sparse-checkout (`scripts/`, `.github/`,
        // `.git/`); we don't want those to masquerade as banks. Banks
        // that already hold .t5p files are picked up above regardless of
        // marker presence.
        const auto userRoot = PresetFormat::getUserPresetsDirectory();
        for (auto& d : userRoot.findChildFiles(juce::File::findDirectories, false, "*"))
        {
            const auto name = d.getFileName();
            if (name.startsWithChar('.')) continue;          // .git / .github / hidden
            if (banks.count(name) > 0)    continue;          // already discovered via .t5p
            if (d.getChildFile(".t5ynth-bank").existsAsFile())
                banks.insert(name);
        }

        sidebar.setVocabulary({ banks.begin(),  banks.end()  },
                              { models.begin(), models.end() },
                              { tags.begin(),   tags.end()   });
        // The cloud taxonomy is the union of MainPanel's canonical set and
        // whatever user-tags the library currently contains — refresh both
        // whenever either input changes.
        applyTagVocabulary();
    }

    /** Build `sidebar-user-tags ∪ canonical` and push to Detail. The result
     *  is also cached so enterSaveMode() can pass an identical vector to
     *  the SaveDrawer (both clouds must display the same set).
     *
     *  The merged set is sorted alphabetically (case-insensitive) so the
     *  cloud reads as one predictable list — previously user tags and the
     *  canonical taxonomy kept their own separate orders (alphabetical,
     *  then curated-by-category), which read as neither. The clouds still
     *  clip after a few rows (silent break in paintTagCloud), but which
     *  tags survive the clip is no longer a function of tag source. */
    void applyTagVocabulary()
    {
        const auto& userTags = sidebar.getTagVocabulary();
        std::vector<juce::String> merged;
        merged.reserve(userTags.size() + (size_t) canonicalTagVocabulary.size());
        std::set<juce::String> seen;
        auto push = [&](const juce::String& s)
        {
            const auto key = s.trim().toLowerCase();
            if (key.isEmpty() || seen.count(key)) return;
            seen.insert(key);
            merged.push_back(s.trim());
        };
        for (auto& u : userTags)               push(u);
        for (auto& c : canonicalTagVocabulary) push(c);
        std::sort(merged.begin(), merged.end(), [](const juce::String& a, const juce::String& b)
        {
            return a.compareIgnoreCase(b) < 0;
        });
        mergedTagVocabulary = merged;
        detail.setTagVocabulary(merged);
    }

    Entry parseEntry(const juce::File& file)
    {
        Entry e;
        e.file = file;
        e.name = file.getFileNameWithoutExtension();
        e.modified = file.getLastModificationTime();
        e.bank = computeBankLabel(file);

        // Read header — only the JSON section, not the audio PCM
        juce::FileInputStream in(file);
        if (! in.openedOk()) return e;
        char magic[4];
        if (in.read(magic, 4) != 4) return e;
        if (juce::String(magic, 4) != "T5YN") return e;
        const auto version = (uint32_t) in.readInt();
        juce::ignoreUnused(version);
        const auto jsonLen = (uint32_t) in.readInt();
        if (jsonLen == 0 || jsonLen > 8 * 1024 * 1024) return e;
        juce::MemoryBlock jsonBlk;
        if (in.readIntoMemoryBlock(jsonBlk, (ssize_t) jsonLen) != (ssize_t) jsonLen) return e;
        const juce::String json(static_cast<const char*>(jsonBlk.getData()),
                                static_cast<size_t>(jsonBlk.getSize()));
        const auto parsed = juce::JSON::parse(json);
        const auto* root = parsed.getDynamicObject();
        if (root == nullptr) return e;

        const auto storedName = root->getProperty("name").toString().trim();
        if (storedName.isNotEmpty() && storedName != "T5ynth Export")
            e.name = storedName;

        if (auto* synth = root->getProperty("synth").getDynamicObject())
        {
            e.promptA = synth->getProperty("promptA").toString();
            e.promptB = synth->getProperty("promptB").toString();
            const auto rawModel = synth->getProperty("model").toString().trim();
            e.model = shortenModelName(rawModel);
            // The language oscillator's stored id was "LCO" before it was named
            // the Language-Resonant Oscillator. Files keep whichever token they
            // were written with; the card shows the current name for both, so an
            // older preset does not read as "LRO · LCO" in the MODEL row.
            if (e.model == "LCO") e.model = "LRO";
            e.device = synth->getProperty("device").toString().trim();
            // synth.inferenceMs is optional — added to format after the
            // initial v3 release; older presets simply have 0.
            e.inferenceMs = (float) (double) synth->getProperty("inferenceMs");
        }

        if (auto* eng = root->getProperty("engine").getDynamicObject())
        {
            const auto m = eng->getProperty("mode").toString();
            // "csound" is what the live language oscillator writes today,
            // "lco" what the retired wavetable-bake path wrote; both are the
            // LRO to the user, who never sees either token.
            const bool isLco = (m == "lco" || m == "csound");
            if (m == "wavetable")    e.engineMode = "Wavetable";
            else if (m == "freeze")  e.engineMode = "Granular";
            else if (m == "sampler") e.engineMode = "Sampler";
            else if (isLco)          e.engineMode = "LRO";
            else if (m.isNotEmpty()) e.engineMode = m;

            // The MODEL section of the sidebar files a preset by what MADE its
            // sound, and for an LRO preset that is the oscillator, not an
            // inference model — the voices render the orchestra whatever
            // `synth.model` says. A bake runs no model of its own, so that
            // field is empty in every LRO preset saved without a preceding
            // generation, and the entire LRO half of a library had no sidebar
            // row at all. Derived on READ, so presets already on disk get
            // their row without being re-saved, and derived into a field of
            // its own so the Detail card keeps reporting the id the file
            // actually carries.
            if (isLco) e.sourceLabel = "LRO";

            // The prompt that wrote the orchestra. Lives here, not in
            // `synth`, because the LRO's prompt editor is not the neural
            // one — see Entry::lroPrompt. Read for every preset that has
            // the field; only an LRO preset ever displays it.
            e.lroPrompt = eng->getProperty("csound_prompt").toString().trim();
        }

        // Non-LRO presets never claim the LRO row. `lastModel` — which is what
        // lands in `synth.model` — is left holding the oscillator's token after
        // an LRO preset is loaded (MainPanel::loadPresetFromFile) and nothing
        // clears it when the engine is switched back, so a neural preset saved in
        // that state would otherwise walk into the row as a lookalike. It keeps
        // its file-level claim in the Detail card; it just doesn't get filed
        // under an oscillator it doesn't use. (Both the old "LCO" and the new
        // "LRO" token arrive here as "LRO" — normalised above.)
        if (e.sourceLabel.isEmpty() && e.model != "LRO")
            e.sourceLabel = e.model;

        // Sequencer mode = combination of step seq + generative seq enables.
        // The fields land in two different JSON blocks, so we derive a
        // single human-readable string here.
        bool stepEnabled = false, genEnabled = false;
        if (auto* seq = root->getProperty("sequencer").getDynamicObject())
            stepEnabled = (bool) seq->getProperty("enabled");
        if (auto* gseq = root->getProperty("generativeSeq").getDynamicObject())
            genEnabled  = (bool) gseq->getProperty("enabled");
        if (stepEnabled && genEnabled) e.seqMode = "Step + Gen";
        else if (stepEnabled)          e.seqMode = "Step";
        else if (genEnabled)           e.seqMode = "Gen";
        else                           e.seqMode = "Off";

        if (auto* arr = root->getProperty("tags").getArray())
            for (auto& v : *arr)
            {
                auto t = v.toString().trim();
                if (t.isNotEmpty()) e.tags.addIfNotAlreadyThere(t);
            }

        if (auto* axesArr = root->getProperty("semanticAxes").getArray())
        {
            bool anyAssigned = false;
            for (int i = 0; i < std::min(axesArr->size(), 3); ++i)
                if (auto* ax = (*axesArr)[i].getDynamicObject())
                {
                    const int dropId = (int) ax->getProperty("dropdownId");
                    // Map across BOTH model tables — a preset can carry SAO ids
                    // (2-9) or SA3 ids (10-12), and the ids are not a contiguous
                    // 1..N range, so index-into-getAxisLabels() would miss SA3.
                    const auto label = AxesPanel::displayForAxisId(dropId);
                    e.axes[(size_t) i] = { label, (float) (double) ax->getProperty("value") };
                    if (! label.isEmpty() && label != "---") anyAssigned = true;
                }
            // Only show the AXES section if at least one slot is actually used.
            e.hasAxes = anyAssigned;
        }

        if (auto* meta = root->getProperty("audio_meta").getDynamicObject())
        {
            e.sampleRate  = (double) meta->getProperty("sampleRate");
            e.numChannels = (int)    meta->getProperty("channels");
            e.numSamples  = (int)    meta->getProperty("numSamples");
        }

        return e;
    }

    /** Find which entry in `allEntries` (if any) corresponds to the path
     *  the Save-Drawer is currently targeting for an overwrite. Result is
     *  cached in `conflictEntryIndex` and consumed by paintListBoxItem to
     *  paint a red overlay on that row. Called whenever the drawer fires
     *  onConflictChanged. */
    void computeConflictRow()
    {
        conflictEntryIndex = -1;
        if (currentMode != Mode::Save) return;
        const auto key = saveDrawer.getConflictPathKey();
        if (key.isEmpty()) return;
        const auto userDir = PresetFormat::getUserPresetsDirectory();
        for (size_t i = 0; i < allEntries.size(); ++i)
        {
            const auto& e = allEntries[i];
            if (! e.file.isAChildOf(userDir)) continue;
            const auto rel = e.file.getRelativePathFrom(userDir).replace("\\", "/").toLowerCase();
            if (rel == key) { conflictEntryIndex = (int) i; return; }
        }
    }

    /** Auto-fill the SaveDrawer's tag chips from the save target. When the
     *  current (name, bank) tuple would overwrite an existing preset, the
     *  chips mirror THAT file's on-disk tags so an overwrite-save keeps its
     *  curation; a fresh name starts with no tags at all.
     *
     *  This replaces the old lastTags prefill, which copied the previously
     *  LOADED preset's tags into every save regardless of target — whole
     *  families of unrelated presets ended up sharing one inherited tag
     *  ('perc' on filter sweeps, 'ambient' on everything saved after an
     *  ambient patch, …).
     *
     *  Runs on every conflict-state change (name keystrokes, bank picks,
     *  save-mode row retargets). SaveDrawer::setTagsAuto() ignores the
     *  call once the user has hand-edited the chip set, so manual curation
     *  survives subsequent retargeting. */
    void refreshSaveDrawerAutoTags()
    {
        if (currentMode != Mode::Save) return;
        saveDrawer.setTagsAuto(
            juce::isPositiveAndBelow(conflictEntryIndex, (int) allEntries.size())
                ? allEntries[(size_t) conflictEntryIndex].tags
                : juce::StringArray());
    }

    void rebuildFiltered()
    {
        filteredIndices.clear();
        const auto needle = searchEditor.getText().trim().toLowerCase();

        for (size_t i = 0; i < allEntries.size(); ++i)
        {
            const auto& e = allEntries[i];

            // Bank filter (empty active = show all)
            if (! sidebar.activeBank.isEmpty() && e.bank != sidebar.activeBank)
                continue;

            // Model filter (XOR — empty active = show all)
            if (currentMode != Mode::Save && ! sidebar.activeModel.isEmpty() && e.sourceLabel != sidebar.activeModel)
                continue;

            // Tag filter (empty → all; otherwise need any-of-match)
            if (currentMode != Mode::Save && ! sidebar.selectedTags.empty())
            {
                bool any = false;
                for (auto& t : e.tags)
                    if (sidebar.selectedTags.count(t)) { any = true; break; }
                if (! any) continue;
            }

            // Search needle
            if (needle.isNotEmpty())
            {
                const bool nameHit    = e.name.toLowerCase().contains(needle);
                const bool promptHit  = e.searchablePrompt().toLowerCase().contains(needle);
                if (! nameHit && ! promptHit) continue;
            }

            filteredIndices.push_back((int) i);
        }

        // All auto-select paths below are programmatic: filter rebuilds,
        // currentPreset restoration and the row-0 fallback are not user
        // gestures, so they must not retarget the SaveDrawer in Save
        // mode. The ScopedValueSetter guards selectedRowsChanged.
        //
        // Hoisted ABOVE updateContent() because updateContent() can
        // itself fire selectedRowsChanged synchronously if the row count
        // shrinks and the ListBox clamps the selection. Today the ListBox
        // is single-selection so a clamp yields -1 and selectedRowsChanged
        // early-returns -- but if anyone ever enables multi-selection,
        // an unguarded clamp here would silently clobber the typed Save
        // name. Cheap insurance.
        juce::ScopedValueSetter<bool> sv(programmaticSelection, true);
        presetList.updateContent();

        // Restore priority: the entry the user explicitly selected wins
        // over the currently-loaded preset — a sidebar/search refilter
        // must not yank the selection (and the Detail card) away from
        // what the user is looking at just because the loaded preset is
        // also visible somewhere in the new list.
        if (selectedEntryIndex >= 0)
        {
            for (size_t i = 0; i < filteredIndices.size(); ++i)
                if (filteredIndices[i] == selectedEntryIndex)
                {
                    selectRowAndSyncDetail((int) i);
                    return;
                }
        }
        for (size_t i = 0; i < filteredIndices.size(); ++i)
        {
            const auto& e = allEntries[(size_t) filteredIndices[i]];
            if (e.file == currentFile)
            {
                selectRowAndSyncDetail((int) i);
                return;
            }
        }
        if (! filteredIndices.empty())
            selectRowAndSyncDetail(0);
        else
        {
            selectedEntryIndex = -1;
            detail.clear();
        }
    }

    // ─── Sidebar pane ────────────────────────────────────────────────────
    class Sidebar : public juce::Component,
                    private juce::Timer
    {
    public:
        /** Bank filter — empty string means "all banks". */
        juce::String activeBank;
        /** Model filter — empty string means "all models" (XOR with the
         *  rows in the MODEL section; clicking the active row deselects). */
        juce::String activeModel;
        /** Tags are exclusive by default; Shift-click adds/removes tags. */
        std::set<juce::String> selectedTags;
        std::function<void()> onChanged;
        std::function<void()> onCreateBankRequested;

        /** BLOCKING per CLAUDE.md: every juce::Timer subclass must
         *  stopTimer() in its destructor before any member is destroyed. */
        ~Sidebar() override { stopTimer(); }

        void setVocabulary(std::vector<juce::String> banks,
                           std::vector<juce::String> models,
                           std::vector<juce::String> tags)
        {
            bankEntries  = std::move(banks);
            modelEntries = std::move(models);
            tagEntries   = std::move(tags);

            // Drop selections that no longer exist
            if (! activeBank.isEmpty()
                && std::find(bankEntries.begin(), bankEntries.end(), activeBank) == bankEntries.end())
                activeBank.clear();
            if (! activeModel.isEmpty()
                && std::find(modelEntries.begin(), modelEntries.end(), activeModel) == modelEntries.end())
                activeModel.clear();
            for (auto it = selectedTags.begin(); it != selectedTags.end(); )
                it = std::find(tagEntries.begin(), tagEntries.end(), *it) == tagEntries.end()
                       ? selectedTags.erase(it) : std::next(it);

            rebuildLayout();
            repaint();
        }

        const std::vector<juce::String>& getTagVocabulary() const noexcept { return tagEntries; }

        void setSaveMode(bool shouldBeSaveMode)
        {
            saveMode = shouldBeSaveMode;
            if (saveMode)
            {
                activeModel.clear();
                selectedTags.clear();
            }
            rebuildLayout();
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            if (items.empty())
                rebuildLayout();   // defensive: paint before first resized()
            for (const auto& it : items)
            {
                if (it.kind == Item::Kind::Header)
                {
                    g.setColour(kDim);
                    g.setFont(juce::FontOptions(kUiLabelFontMin, juce::Font::bold));
                    g.drawText(it.label, it.rect, juce::Justification::centredLeft, false);
                    g.setColour(kBorder.withAlpha(0.5f));
                    g.drawLine((float) it.rect.getX(), (float) it.rect.getBottom() - 1.0f,
                               (float) it.rect.getRight(), (float) it.rect.getBottom() - 1.0f, 0.5f);
                    // "+" next to BANK is now a standalone disk action
                    // (create empty bank), no longer gated on Save mode —
                    // the user can organise banks any time.
                    if (it.label == "BANK" && ! addBankRect.isEmpty())
                    {
                        g.setColour(kAccent.withAlpha(0.16f));
                        g.fillRoundedRectangle(addBankRect.toFloat(), 3.0f);
                        g.setColour(kAccent);
                        g.drawRoundedRectangle(addBankRect.toFloat(), 3.0f, 1.0f);
                        g.setColour(juce::Colours::white);
                        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
                        g.drawText("+", addBankRect, juce::Justification::centred, false);
                    }
                }
                else
                {
                    const bool active = isItemActive(it);
                    if (active)
                    {
                        g.setColour(kAccent.withAlpha(0.18f));
                        g.fillRect(it.rect);
                        g.setColour(kAccent);
                        g.fillRect(juce::Rectangle<int>(it.rect.getX(), it.rect.getY(), 3, it.rect.getHeight()));
                    }
                    g.setColour(juce::Colours::white);
                    g.setFont(juce::FontOptions(12.0f));
                    auto txt = it.rect.withTrimmedLeft(8);
                    // Bank/Model entries with an empty label are the "show all" marker.
                    const bool isAllRow = it.label.isEmpty()
                        && (it.kind == Item::Kind::Bank || it.kind == Item::Kind::Model);
                    g.drawText(isAllRow ? juce::String("All") : it.label,
                               txt, juce::Justification::centredLeft, true);
                }
            }
        }

        void resized() override
        {
            rebuildLayout();
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            // Reset any leftover long-press state — guards against an
            // orphaned timer if the previous gesture ended outside the
            // component (mouseUp without mouseExit→mouseUp).
            stopTimer();
            pressedItemKind  = Item::Kind::Header;
            pressedItemLabel.clear();
            pressedAdditive  = e.mods.isShiftDown();
            longPressFired   = false;

            const auto p = e.getPosition();
            if (addBankRect.contains(p))
            {
                if (onCreateBankRequested) onCreateBankRequested();
                return;
            }

            for (const auto& it : items)
            {
                if (it.kind == Item::Kind::Header) continue;
                if (! it.rect.contains(p)) continue;

                // Tag rows in library/browse mode get the long-press-for-drag
                // treatment: the filter action is deferred to mouseUp so that
                // a long press (~`longPressMs`) can arm a DragAndDrop session
                // without also toggling the filter on release. Bank/Model rows
                // keep their immediate-on-mouseDown response — they don't drag
                // and deferring would just feel sluggish.
                if (it.kind == Item::Kind::Tag && ! saveMode)
                {
                    pressedItemKind  = it.kind;
                    pressedItemLabel = it.label;
                    startTimer(longPressMs);
                    return;
                }

                handleHit(it, e.mods.isShiftDown());
                repaint();
                if (onChanged) onChanged();
                return;
            }
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (pressedItemKind != Item::Kind::Tag) return;
            if (pressedItemLabel.isEmpty())       return;
            // Mouse-slop threshold — small wiggles shouldn't cancel a
            // pending long-press OR start a drag.
            if (e.getDistanceFromDragStart() < 4) return;

            if (! longPressFired)
            {
                // The user moved before the long-press timer fired —
                // treat as a cancelled gesture: no filter toggle, no drag.
                // Prevents accidental filter changes from twitchy clicks.
                stopTimer();
                pressedItemLabel.clear();
                return;
            }

            if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
            {
                // Render a chip-sized preview rather than letting JUCE auto-
                // snapshot the entire Sidebar (which would float a giant
                // ~150px-wide ghost of the whole row column). Suggestion
                // style = outline-only, matching how the same tag looks in
                // Detail's "Known tags" cloud — visual continuity for
                // "this is a tag I'm dragging".
                const int chipW = juce::Font(juce::FontOptions(11.0f))
                                      .getStringWidth(pressedItemLabel) + 14;
                const int chipH = 20;
                juce::Image dragImg(juce::Image::ARGB, chipW + 2, chipH + 2, true);
                {
                    juce::Graphics dg(dragImg);
                    PresetManagerPanel::paintTagChip(
                        dg, 1, 1, pressedItemLabel, ChipKind::Suggestion);
                }
                container->startDragging(juce::var(pressedItemLabel),
                                         this,
                                         juce::ScaledImage(dragImg));
                // Consumed — further mouseDrag firings during this gesture
                // are harmless since the JUCE drag session owns the cursor
                // until release.
                pressedItemLabel.clear();
                longPressFired = false;
            }
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            stopTimer();
            if (pressedItemKind != Item::Kind::Tag || pressedItemLabel.isEmpty())
            {
                pressedItemLabel.clear();
                longPressFired = false;
                return;
            }

            // Reaching mouseUp with the press-label still set means no drag
            // session was actually started — startDragging() clears the label
            // inline, and the twitchy-click bail in mouseDrag clears it too.
            // So treat any surviving mouseUp as a click and fire the deferred
            // filter toggle. This includes the "user long-pressed but never
            // moved" case: rather than swallowing the gesture silently (which
            // surprised testers — they expected the filter to still apply
            // after a slow click), we fall through to the same toggle the
            // short-click path uses.
            for (const auto& it : items)
            {
                if (it.kind == Item::Kind::Tag && it.label == pressedItemLabel)
                {
                    handleHit(it, pressedAdditive);
                    repaint();
                    if (onChanged) onChanged();
                    break;
                }
            }

            pressedItemLabel.clear();
            longPressFired = false;
        }

    private:
        // Declared up here (rather than alongside the other layout state)
        // because `pressedItemKind` below depends on `Item::Kind` — the
        // long-press members must follow this type.
        struct Item
        {
            enum class Kind { Header, Bank, Model, Tag };
            Kind kind;
            juce::Rectangle<int> rect;
            juce::String label;     // for Bank / Model / Tag this IS the value
        };

        void timerCallback() override
        {
            stopTimer();
            longPressFired = true;
            // No visual flash for the armed state — the drag session itself
            // gives immediate feedback as soon as the user starts moving.
            // Adding a highlight here would also re-enter paint() and
            // require tracking which row is armed; not worth the surface.
        }

        // Long-press threshold. ~350 ms is a comfortable middle ground:
        // long enough that fast filter-clicks don't accidentally arm a
        // drag, short enough that "press and grab" still feels direct.
        static constexpr int longPressMs = 350;

        // Long-press / drag state — populated on mouseDown, consumed on
        // mouseDrag / mouseUp / timerCallback. None of these need to
        // survive a destructor (stopTimer() in dtor cancels any pending
        // callback first, so the members are safe to be destroyed after).
        Item::Kind   pressedItemKind = Item::Kind::Header;
        juce::String pressedItemLabel;
        bool         pressedAdditive = false;
        bool         longPressFired  = false;

        std::vector<Item> items;
        std::vector<juce::String> bankEntries;
        std::vector<juce::String> modelEntries;
        std::vector<juce::String> tagEntries;
        juce::Rectangle<int> addBankRect;
        bool saveMode = false;

        void rebuildLayout()
        {
            items.clear();
            addBankRect = {};
            auto area = getLocalBounds().reduced(8);
            const int rowH = 22;
            const int headerH = 18;
            const int gap = 12;

            auto pushHeader = [&](const char* label, bool withAdd = false)
            {
                auto r = area.removeFromTop(headerH);
                if (withAdd)
                {
                    addBankRect = r.removeFromRight(headerH).reduced(1);
                    r.removeFromRight(4);
                }
                items.push_back({ Item::Kind::Header, r, label });
                area.removeFromTop(2);
            };
            auto pushRow = [&](Item::Kind kind, const juce::String& label)
            {
                if (area.getHeight() < rowH) return;
                auto r = area.removeFromTop(rowH);
                items.push_back({ kind, r, label });
            };

            pushHeader("BANK", /*withAdd*/ true);
            // "All" is represented by an empty `label` so activeBank.isEmpty()
            // means "no filter".
            pushRow(Item::Kind::Bank, "");
            for (auto& b : bankEntries) pushRow(Item::Kind::Bank, b);

            if (! saveMode && ! modelEntries.empty())
            {
                area.removeFromTop(gap);
                pushHeader("MODEL");
                // "All" sentinel for the Model section too — empty label
                // means "no model filter". Same XOR semantics as Bank.
                pushRow(Item::Kind::Model, "");
                for (auto& m : modelEntries) pushRow(Item::Kind::Model, m);
            }

            if (! saveMode && ! tagEntries.empty())
            {
                area.removeFromTop(gap);
                pushHeader("TAGS");
                for (auto& t : tagEntries) pushRow(Item::Kind::Tag, t);
            }
        }

        bool isItemActive(const Item& it) const
        {
            switch (it.kind)
            {
                case Item::Kind::Header: return false;
                case Item::Kind::Bank:   return activeBank  == it.label;
                case Item::Kind::Model:  return activeModel == it.label;
                case Item::Kind::Tag:    return selectedTags.count(it.label) > 0;
            }
            return false;
        }

        void handleHit(const Item& it, bool additiveTagSelection)
        {
            switch (it.kind)
            {
                case Item::Kind::Header: break;
                case Item::Kind::Bank:
                    // XOR with toggle-off: clicking the active row deselects.
                    activeBank  = (activeBank  == it.label) ? juce::String() : it.label;
                    break;
                case Item::Kind::Model:
                    activeModel = (activeModel == it.label) ? juce::String() : it.label;
                    break;
                case Item::Kind::Tag:
                    if (additiveTagSelection)
                    {
                        if (selectedTags.erase(it.label) == 0)
                            selectedTags.insert(it.label);
                    }
                    else
                    {
                        const bool wasOnlySelected = selectedTags.size() == 1
                                                  && selectedTags.count(it.label) > 0;
                        selectedTags.clear();
                        if (! wasOnlySelected)
                            selectedTags.insert(it.label);
                    }
                    break;
            }
        }
    };
    Sidebar sidebar;

    // ─── Detail pane ─────────────────────────────────────────────────────
    class Detail : public juce::Component,
                   public juce::DragAndDropTarget
    {
    public:
        std::function<void()> onLoadRequested;
        std::function<void(const juce::StringArray&)> onTagsCommitted;

        Detail()
        {
            configureBtn(loadBtn, kAccent);
            loadBtn.onClick = [this] { if (onLoadRequested) onLoadRequested(); };
            addAndMakeVisible(loadBtn);

            tagInput.setTextToShowWhenEmpty("+ tag", kDimmer);
            tagInput.setColour(juce::TextEditor::backgroundColourId, kSurface.brighter(0.05f));
            tagInput.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            tagInput.setColour(juce::TextEditor::outlineColourId, kBorder);
            tagInput.setColour(juce::TextEditor::focusedOutlineColourId, kAccent);
            tagInput.setEscapeAndReturnKeysConsumed(false);
            tagInput.onReturnKey = [this]
            {
                const auto t = tagInput.getText().trim();
                if (t.isEmpty()) return;
                // Case-insensitive add matches the cloud-click and drag-drop
                // paths — without this "Ambient" and "ambient" could coexist
                // in the same preset.
                tags.addIfNotAlreadyThere(t, true);
                tagInput.setText({}, false);
                if (onTagsCommitted) onTagsCommitted(tags);
                refreshCloud();
                resized();
                repaint();
            };
            // Autocomplete: typing in the field filters the "Known tags" cloud
            // to entries whose prefix matches. Cheap full repaint — the cloud
            // is at most ~50 chips and we already repaint on every chip
            // change. Doing it here keeps the cloud and input field visually
            // linked without a separate popup component.
            tagInput.onTextChange = [this] { refreshCloud(); repaint(); };
            addAndMakeVisible(tagInput);

            cloud.onTagPicked = [this] (const juce::String& t)
            {
                // Belt-and-braces: in Save mode this card shows the preset
                // that would be REPLACED, so committing a tag here would
                // rewrite the wrong file's tags on disk. resized() hides the
                // cloud in that mode; this guard makes the invariant local.
                if (! browseActionsVisible || ! entryValid) return;
                tags.addIfNotAlreadyThere(t, true);
                // Clear the autocomplete filter so the cloud immediately
                // shows the next pickable tag rather than a stale prefix.
                tagInput.setText({}, false);
                if (onTagsCommitted) onTagsCommitted(tags);
                refreshCloud();
                resized();
                repaint();
            };
            addChildComponent(cloud);   // visibility decided in resized()
        }

        /** Forward Esc keystrokes from the tag editor to the panel-level
         *  KeyListener so the overlay can be dismissed even while typing. */
        void installEscListener(juce::KeyListener* l)
        {
            tagInput.addKeyListener(l);
        }

        /** Curated + user-seen tag set rendered as a click-to-add cloud
         *  below the active chips. Empty vector hides the cloud entirely.
         *  See PresetManagerPanel::applyTagVocabulary() for the merge. */
        void setTagVocabulary(std::vector<juce::String> v)
        {
            cloud.setVocabulary(std::move(v));
            refreshCloud();
            resized();
            repaint();
        }

        void clear()
        {
            entryValid = false;
            name.clear();
            bank.clear();
            promptA.clear(); promptB.clear(); lroPrompt.clear();
            engineMode.clear();
            hasAxes = false;
            sampleRate = 0.0; numChannels = 0; numSamples = 0;
            tags.clear();
            refreshCloud();
            updateButtonsEnabled();
            resized();
            repaint();
        }

        void setEntry(const Entry& e)
        {
            entryValid = true;
            name = e.name;
            bank = e.bank;
            modified = e.modified;
            promptA = e.promptA;
            promptB = e.promptB;
            lroPrompt = e.lroPrompt;
            hasAxes = e.hasAxes;
            axes = e.axes;
            sampleRate  = e.sampleRate;
            numChannels = e.numChannels;
            numSamples  = e.numSamples;
            inferenceMs = e.inferenceMs;
            model       = e.model;
            sourceLabel = e.sourceLabel;
            device      = e.device;
            engineMode  = e.engineMode;
            seqMode     = e.seqMode;
            tags = e.tags;
            cachedPromptW = -1;    // force recompute on next paint
            refreshCloud();
            updateButtonsEnabled();
            resized();
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            paintCard(g, getLocalBounds());

            // Tag-drop hover overlay — accent-tinted border around the whole
            // card, mirroring SaveDrawer's chip-area highlight idiom so the
            // two drop targets read as the same affordance family. Drawn
            // before content so the chip / text layers paint on top.
            if (dropHover && entryValid)
            {
                auto r = getLocalBounds().toFloat().reduced(1.5f);
                g.setColour(kAccent.withAlpha(0.10f));
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(kAccent.withAlpha(0.55f));
                g.drawRoundedRectangle(r, 4.0f, 1.5f);
            }

            if (! entryValid)
            {
                g.setColour(kDim);
                g.setFont(juce::FontOptions(13.0f));
                g.drawText("No preset selected", getLocalBounds(), juce::Justification::centred);
                return;
            }

            const auto L = computeCardLayout();

            // Title
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
            g.drawText(name, L.title, juce::Justification::centredLeft, true);

            // Subline
            g.setColour(kDim);
            g.setFont(juce::FontOptions(11.0f));
            g.drawText(bank + (modified.toMilliseconds() > 0
                                 ? juce::String::fromUTF8(" \xc2\xb7 ") + modified.formatted("%Y-%m-%d %H:%M")
                                 : juce::String()),
                       L.subline, juce::Justification::centredLeft, false);

            // ── DEVICE row — above MODEL. The audio is reproducible only on the
            //    device it was rendered on (MPS/CUDA/CPU diverge for the same
            //    seed). A preset from a different device type than this machine's
            //    is flagged pink: its prompts and synth settings still transfer,
            //    only the regenerated audio will differ. Empty = legacy preset
            //    (no claim) → neutral "—", like the other unknown meta rows. ──
            const bool deviceMismatch = device.isNotEmpty()
                                     && runtimeDevice.isNotEmpty()
                                     && ! device.equalsIgnoreCase(runtimeDevice);
            paintMetaRow(g, L.device, "DEVICE",
                         device.toUpperCase(),
                         deviceMismatch ? kAccent : juce::Colours::white);

            // ── META block — compact label/value rows. Always rendered so the
            //    detail card has a stable shape regardless of which fields a
            //    given preset happens to expose. ──
            const std::pair<const char*, juce::String> metaRows[] = {
                { "MODEL",  modelRowText() },
                { "ENGINE", engineMode },
                { "SEQ",    seqMode },
                { "AUDIO",  audioInfoString() },
                { "INFER",  inferenceString() },
            };
            for (size_t i = 0; i < L.meta.size(); ++i)
                paintMetaRow(g, L.meta[i], metaRows[i].first, metaRows[i].second);

            paintSection(g, L.promptA, L.promptATitle, L.promptABody);
            if (L.showPromptB)
                paintSection(g, L.promptB, "IMPULSE B", promptB);
            if (hasAxes)
                paintAxes(g, L.axes);

            paintTagChips(g, L.tags);
            // The "Known tags" cloud is a child component (see TagCloud) and
            // paints itself — resized() gave it L.cloud.
        }

        void resized() override
        {
            loadBtn.setVisible(browseActionsVisible);
            tagInput.setVisible(browseActionsVisible);

            const auto L = computeCardLayout();
            // Hidden rather than empty-bounded when there is nothing to offer,
            // no room for it, or no preset selected — paint() draws "No preset
            // selected" over the whole card in that last case and a live chip
            // flow on top of it would be a click target for a preset that is
            // not there.
            cloud.setVisible(browseActionsVisible && entryValid && ! L.cloud.isEmpty());
            if (! L.cloud.isEmpty())
                cloud.setBounds(L.cloud);

            if (! browseActionsVisible) return;

            // Action strip at bottom — Load is the only primary action.
            // Delete / Rename live in the right-click context menu so that
            // the destructive action can never be hit by accident from a
            // mis-aimed Load click.
            loadBtn.setBounds(L.actionStrip);

            // The tag chips are painted by paint(); only the input editor
            // (its own row, right-aligned) needs Component bounds.
            const int tagInputW = juce::jmin(120, getLocalBounds().getWidth() / 3);
            auto inputRow = L.tagInputRow;
            tagInput.setBounds(inputRow.removeFromRight(tagInputW));
        }

        /** Toggled by the owner when entering / leaving Save mode. While
         *  off, chip clicks behave exactly as before (× to remove). While
         *  on, a chip drag starts a DragAndDrop session whose description
         *  is the tag string. */
        void setDragSourceEnabled(bool e) { dragSourceEnabled = e; }

        /** This machine's backend inference device, for the cross-device flag. */
        void setRuntimeDevice(const juce::String& dev)
        {
            if (runtimeDevice == dev) return;
            runtimeDevice = dev;
            repaint();
        }

        /** Browse-only actions are hidden while the Save drawer is open. */
        void setBrowseActionsVisible(bool visible)
        {
            browseActionsVisible = visible;
            updateButtonsEnabled();
            resized();
            repaint();
        }

        // ── DragAndDropTarget ──
        // Accepts tag drops from the Sidebar's TAGS rows (long-press → drag,
        // see Sidebar::mouseDown/mouseDrag). The drop adds the tag to the
        // currently-displayed preset's set and fires onTagsCommitted so the
        // owner can persist it to disk (matches the tagInput / cloud-click
        // paths — single mutation funnel, case-insensitive dedup).
        //
        // Gated on `browseActionsVisible && entryValid`:
        //   * Save mode: this card shows the preset that will be REPLACED,
        //     not the save target — silently rewriting its on-disk tags via
        //     onTagsCommitted would be a destructive surprise. SaveDrawer
        //     carries its own drop target for the save composition.
        //   * No preset selected: no `tags` to mutate.
        bool isInterestedInDragSource(const SourceDetails& d) override
        {
            return browseActionsVisible
                && entryValid
                && d.description.isString()
                && d.description.toString().trim().isNotEmpty();
        }

        void itemDragEnter(const SourceDetails&) override
        {
            dropHover = true;
            repaint();
        }

        void itemDragExit(const SourceDetails&) override
        {
            dropHover = false;
            repaint();
        }

        void itemDropped(const SourceDetails& d) override
        {
            dropHover = false;
            if (! browseActionsVisible || ! entryValid) { repaint(); return; }
            const auto tag = d.description.toString().trim();
            if (tag.isNotEmpty())
            {
                tags.addIfNotAlreadyThere(tag, true);
                if (onTagsCommitted) onTagsCommitted(tags);
                refreshCloud();
                resized();
            }
            repaint();
        }

    private:
        static void configureBtn(juce::TextButton& b, juce::Colour c)
        {
            b.setColour(juce::TextButton::buttonColourId, c);
            b.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        }

        void updateButtonsEnabled()
        {
            loadBtn.setEnabled(browseActionsVisible && entryValid);
            tagInput.setEnabled(browseActionsVisible && entryValid);
        }

        /** Push the two things that decide what the cloud may offer: the tags
         *  already on this preset, and the input field's prefix filter. */
        void refreshCloud() { cloud.update(tags, tagInput.getText()); }

        /** Every rectangle the card is made of. */
        struct CardLayout
        {
            juce::Rectangle<int> title, subline, device;
            std::array<juce::Rectangle<int>, 5> meta;
            juce::Rectangle<int> promptA, promptB, axes, tags, cloud;
            juce::Rectangle<int> tagInputRow, actionStrip;
            bool         showPromptB = false;
            juce::String promptATitle;
            juce::String promptABody;
        };

        /** The card's vertical division, computed ONCE for both consumers:
         *  resized() places the child components into it, paint() draws into
         *  the same rectangles. It used to live inside paint() alone while
         *  resized() re-derived the bottom strip on its own — two copies of
         *  one layout, which is how a component ends up sitting where the
         *  painter never draws. */
        CardLayout computeCardLayout()
        {
            CardLayout L;
            auto area = getLocalBounds().reduced(12);
            L.title   = area.removeFromTop(20);
            L.subline = area.removeFromTop(16);
            area.removeFromTop(10);
            L.device  = area.removeFromTop(16);
            for (auto& m : L.meta) m = area.removeFromTop(16);
            area.removeFromTop(8);

            // Reserve bottom: action strip + tag-input row.
            if (browseActionsVisible)
            {
                L.actionStrip = area.removeFromBottom(32);
                area.removeFromBottom(8);
                L.tagInputRow = area.removeFromBottom(24);
                area.removeFromBottom(6);
            }

            // Impulse-section heights come from the actual wrapped text so
            // there is no leftover gap between a body and the next header.
            // Caps prevent extreme prompts from squeezing the tags. Heights
            // are cached and only recomputed when the available width changes
            // (TextLayout::createLayout is the most expensive call here and it
            // dominates click-to-redraw latency).
            //
            // WHICH prompt follows the engine that sounds. An LRO preset was
            // written by its own prompt editor and gets one PROMPT section;
            // the Impulse pair is the neural panel's and holds whatever was
            // last typed there, so showing it here captions this sound with
            // another one's words. A preset saved before the LRO prompt was
            // persisted has none, and the section says "—" — the file really
            // does not know what was asked for.
            const bool lro = (engineMode == "LRO");
            L.showPromptB  = ! lro;
            L.promptATitle = lro ? "PROMPT" : "IMPULSE A";
            L.promptABody  = lro ? lroPrompt : promptA;

            const int promptW = area.getWidth() - 6;
            const int headerH = 14;
            if (promptW != cachedPromptW)
            {
                cachedPromptHeightA = juce::jlimit(14, 80, wrappedTextHeight(L.promptABody, promptW, 12.0f));
                cachedPromptHeightB = lro ? 0
                                          : juce::jlimit(14, 80, wrappedTextHeight(promptB, promptW, 12.0f));
                cachedPromptW = promptW;
            }

            L.promptA = area.removeFromTop(headerH + cachedPromptHeightA + 4);
            if (L.showPromptB)
            {
                area.removeFromTop(6);
                L.promptB = area.removeFromTop(headerH + cachedPromptHeightB + 4);
            }
            if (hasAxes)
            {
                area.removeFromTop(6);
                L.axes = area.removeFromTop(headerH + 3 * 16);
            }
            area.removeFromTop(8);

            // TAGS section — header + active chips + the "Known tags" cloud.
            // The cloud is omitted when the vocabulary is empty, when not even
            // its header plus one chip row fits on top of a row of active
            // chips, OR when browse actions are hidden (Save mode): there the
            // card shows the preset that would be REPLACED, and a click in it
            // would rewrite that file's tags via onTagsCommitted →
            // onTagsChanged → patchPresetTagsField. The SaveDrawer carries its
            // own cloud for the actual save target.
            L.tags = area.removeFromTop(juce::jmax(headerH + 4, area.getHeight()));
            const int cloudFloor = TagCloud::minimumUsefulHeight();
            if (browseActionsVisible
                && cloud.hasVocabulary()
                && L.tags.getHeight() >= cloudFloor + 26)
            {
                L.cloud = L.tags.removeFromBottom(
                    juce::jlimit(cloudFloor, TagCloud::heightForRows(6), L.tags.getHeight() / 2));
                L.tags.removeFromBottom(4);
            }
            return L;
        }

        /** Compact label/value row used by the META block. */
        static void paintMetaRow(juce::Graphics& g, juce::Rectangle<int> r,
                                 const juce::String& label, const juce::String& value,
                                 juce::Colour valueColour = juce::Colours::white)
        {
            g.setColour(kDim);
            g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            g.drawText(label, r.removeFromLeft(56), juce::Justification::centredLeft, false);
            g.setColour(valueColour);
            g.setFont(juce::FontOptions(11.5f));
            const auto display = value.isEmpty()
                                    ? juce::String::fromUTF8("\xe2\x80\x94")
                                    : value;
            g.drawText(display, r, juce::Justification::centredLeft, true);
        }

        /** MODEL row. It must open with the label the sidebar's MODEL section
         *  files this preset under — the two carry the same word in the same
         *  panel, and a card naming a model whose sidebar row hides the preset
         *  is a contradiction the user cannot resolve. An LRO preset saved
         *  after a generation carries BOTH (its voices sound the orchestra, its
         *  buffer is still the neural sample), so the stored id follows the
         *  oscillator rather than being dropped. Empty when nothing files it:
         *  a legacy preset with no model, or one whose stored id is the stale
         *  oscillator token left in lastModel by a preset load — paintMetaRow draws
         *  the em-dash for both, which is the honest claim. */
        juce::String modelRowText() const
        {
            if (sourceLabel.isEmpty()) return {};
            return (model.isNotEmpty() && model != sourceLabel)
                       ? sourceLabel + juce::String::fromUTF8(" \xc2\xb7 ") + model
                       : sourceLabel;
        }

        juce::String audioInfoString() const
        {
            if (numSamples <= 0 || sampleRate <= 0.0) return {};
            const auto sep = juce::String::fromUTF8(" \xc2\xb7 ");
            const double secs = (double) numSamples / sampleRate;
            juce::String s;
            s << juce::String(secs, secs < 10.0 ? 2 : 1) << " s" << sep
              << juce::String((int) std::round(sampleRate / 1000.0)) << " kHz" << sep
              << (numChannels == 1 ? "mono" : (numChannels == 2 ? "stereo" : juce::String(numChannels) + " ch"));
            return s;
        }

        juce::String inferenceString() const
        {
            if (inferenceMs <= 0.0f) return {};
            const float secs = inferenceMs / 1000.0f;
            return juce::String(secs, secs < 10.0f ? 1 : 0) + " s";
        }

        /** Width-aware wrapped-text height in px, computed via JUCE TextLayout.
         *  Used to size IMPULSE A/B sections to their actual content so there
         *  is no stretched whitespace between them. */
        static int wrappedTextHeight(const juce::String& text, int width, float fontSize)
        {
            if (text.isEmpty()) return juce::roundToInt(fontSize * 1.3f);
            juce::AttributedString s;
            s.append(text, juce::Font(juce::FontOptions(fontSize)), juce::Colours::white);
            s.setWordWrap(juce::AttributedString::byWord);
            s.setJustification(juce::Justification::topLeft);
            juce::TextLayout layout;
            layout.createLayout(s, (float) juce::jmax(20, width));
            return juce::jmax(juce::roundToInt(fontSize * 1.3f),
                              juce::roundToInt(std::ceil(layout.getHeight())));
        }

        void paintSection(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, const juce::String& body)
        {
            g.setColour(kDim);
            g.setFont(juce::FontOptions(kUiLabelFontMin, juce::Font::bold));
            g.drawText(title, r.removeFromTop(14), juce::Justification::centredLeft, false);
            const auto displayBody = body.isEmpty()
                                        ? juce::String::fromUTF8("\xe2\x80\x94")
                                        : body;
            // AttributedString matches the metrics used by wrappedTextHeight,
            // so the section bounds we compute fit the actual rendered glyphs.
            juce::AttributedString s;
            s.append(displayBody, juce::Font(juce::FontOptions(12.0f)), juce::Colours::white);
            s.setWordWrap(juce::AttributedString::byWord);
            s.setJustification(juce::Justification::topLeft);
            s.draw(g, r.toFloat());
        }

        void paintAxes(juce::Graphics& g, juce::Rectangle<int> r)
        {
            g.setColour(kDim);
            g.setFont(juce::FontOptions(kUiLabelFontMin, juce::Font::bold));
            g.drawText("AXES", r.removeFromTop(14), juce::Justification::centredLeft, false);
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(12.0f));
            const auto emDash = juce::String::fromUTF8("\xe2\x80\x94");
            for (int i = 0; i < 3; ++i)
            {
                auto row = r.removeFromTop(16);
                if (row.isEmpty()) break;
                const auto& label = axes[(size_t) i].first;
                // Slot is unset when label == "---" (placeholder) or empty
                const bool unset = label.isEmpty() || label == "---";
                g.drawText(unset ? emDash : label,
                           row.removeFromLeft(row.getWidth() - 60),
                           juce::Justification::centredLeft, true);
                g.drawText(unset ? juce::String() : juce::String(axes[(size_t) i].second, 2),
                           row, juce::Justification::centredRight, false);
            }
        }

        void paintTagChips(juce::Graphics& g, juce::Rectangle<int> r)
        {
            chipRects.clear();
            g.setColour(kDim);
            g.setFont(juce::FontOptions(kUiLabelFontMin, juce::Font::bold));
            g.drawText("TAGS", r.removeFromTop(14), juce::Justification::centredLeft, false);

            if (browseActionsVisible)
                r.removeFromBottom(24);  // input row
            int x = r.getX();
            int y = r.getY();
            const int rowH = 22;
            const int gapX = 4;
            const int gapY = 4;

            for (int i = 0; i < tags.size(); ++i)
            {
                const auto& t = tags[i];
                const int chipW = juce::Font(juce::FontOptions(11.0f)).getStringWidth(t) + 28;
                if (x + chipW > r.getRight())
                {
                    x = r.getX();
                    y += rowH + gapY;
                    if (y + rowH > r.getBottom()) break;
                }
                const auto chip = paintTagChip(g, x, y, t, ChipKind::ActiveRemovable);
                chipRects.push_back({ chip, i });
                x += chipW + gapX;
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            pressedChipIndex = -1;
            const auto p = e.getPosition();

            // Drag-source: any chip can be picked up while save mode is
            // active. The actual juce::DragAndDropContainer::startDragging
            // is deferred until mouseDrag once the press has moved enough
            // to be unambiguously a drag rather than a click.
            if (dragSourceEnabled)
            {
                for (const auto& cr : chipRects)
                    if (cr.bounds.contains(p)) { pressedChipIndex = cr.index; break; }
            }

            // The × hitbox (right ~16 px of each chip) removes the tag.
            for (const auto& cr : chipRects)
            {
                const auto closeBox = juce::Rectangle<int>(
                    cr.bounds.getRight() - 16, cr.bounds.getY(), 16, cr.bounds.getHeight());
                if (closeBox.contains(p))
                {
                    if (cr.index >= 0 && cr.index < tags.size())
                    {
                        tags.remove(cr.index);
                        if (onTagsCommitted) onTagsCommitted(tags);
                        refreshCloud();   // the removed tag is offerable again
                        resized();
                        repaint();
                    }
                    pressedChipIndex = -1;   // × overrides drag intent
                    return;
                }
            }
            // Clicks on the "Known tags" cloud never reach here — it is its
            // own child component and reports picks via cloud.onTagPicked.
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (! dragSourceEnabled || pressedChipIndex < 0) return;
            if (e.getDistanceFromDragStart() < 4) return;
            if (! juce::isPositiveAndBelow(pressedChipIndex, tags.size())) return;
            if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
            {
                container->startDragging(juce::var(tags[pressedChipIndex]), this);
                pressedChipIndex = -1;   // consumed
            }
        }

        void mouseUp(const juce::MouseEvent&) override { pressedChipIndex = -1; }

        bool entryValid = false;
        bool hasAxes = false;
        juce::String name, bank, promptA, promptB, lroPrompt;
        juce::String model, sourceLabel, engineMode, seqMode;
        juce::String device;        // device the audio was rendered on; empty = legacy
        juce::String runtimeDevice; // this machine's backend device, for mismatch flag
        juce::Time modified;
        std::array<std::pair<juce::String, float>, 3> axes;
        double sampleRate = 0.0;
        int numChannels = 0;
        int numSamples = 0;
        float inferenceMs = 0.0f;
        juce::StringArray tags;

        // Cached prompt heights — invalidated on setEntry or when the
        // available paint width changes. Avoids running TextLayout twice
        // per paint frame.
        int cachedPromptW = -1;
        int cachedPromptHeightA = 14;
        int cachedPromptHeightB = 14;

        struct ChipRect { juce::Rectangle<int> bounds; int index; };
        std::vector<ChipRect> chipRects;        // active tags (× to remove)

        int  pressedChipIndex   = -1;
        bool dragSourceEnabled  = false;
        bool browseActionsVisible = true;
        // Tag-drop-from-sidebar hover state — see DragAndDropTarget
        // overrides above. Painted as a subtle accent border around
        // the whole card so the user sees that THIS preset is the drop
        // recipient regardless of where the cursor sits inside it.
        bool dropHover           = false;

        juce::TextButton loadBtn { "Load" };
        juce::TextEditor tagInput;
        TagCloud         cloud;
    };
    Detail detail;

    // ─── Save Drawer ─────────────────────────────────────────────────────
    /** Bottom-of-panel drawer that owns the entire Save workflow when the
     *  panel is in Save mode. Composed of name + bank + tag fields, a
     *  conflict-aware Save button (red "Replace …" when bank+name match an
     *  existing file), Cancel, and a "+ copy" quick-suffix helper. The
     *  drawer is self-contained: it only emits onSaveClicked / onCancelClicked
     *  and exposes getName/getTags/getBank for the owner to consume on save. */
    class SaveDrawer : public juce::Component,
                       public juce::DragAndDropTarget,
                       private juce::KeyListener
    {
    public:
        std::function<void()> onSaveClicked;
        std::function<void()> onCancelClicked;
        /** Fires whenever conflict state changes (name typed, bank picked,
         *  configure called). The owner uses this to repaint the preset
         *  list with the would-replace row highlighted in red. */
        std::function<void()> onConflictChanged;

        SaveDrawer()
        {
            configureLabel(nameLabel);
            nameLabel.setText("Name", juce::dontSendNotification);
            addAndMakeVisible(nameLabel);

            configureEditor(nameEdit);
            nameEdit.setEscapeAndReturnKeysConsumed(false);
            nameEdit.addKeyListener(this);
            nameEdit.onReturnKey  = [this] { commit(); };
            nameEdit.onTextChange = [this] { refreshConflictUi(); };
            addAndMakeVisible(nameEdit);

            configureLabel(warningLabel);
            warningLabel.setColour(juce::Label::textColourId, kWarning);
            warningLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(warningLabel);

            configureLabel(bankLabel);
            bankLabel.setText("Bank", juce::dontSendNotification);
            addAndMakeVisible(bankLabel);

            bankBox.setEditableText(true);
            bankBox.setColour(juce::ComboBox::backgroundColourId, kSurface.brighter(0.04f));
            bankBox.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            bankBox.setColour(juce::ComboBox::outlineColourId, kBorder);
            bankBox.setColour(juce::ComboBox::focusedOutlineColourId, kAccent);
            bankBox.onChange = [this] { refreshConflictUi(); };
            addAndMakeVisible(bankBox);

            includeCacheToggle.setButtonText("Inference-Cache saved");
            includeCacheToggle.setColour(juce::ToggleButton::textColourId, kDim);
            includeCacheToggle.setColour(juce::ToggleButton::tickColourId, kOscCol);
            includeCacheToggle.setVisible(false);
            includeCacheToggle.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(includeCacheToggle);

            configureLabel(tagsLabel);
            tagsLabel.setText("Tags", juce::dontSendNotification);
            addAndMakeVisible(tagsLabel);

            configureEditor(tagInput);
            tagInput.setTextToShowWhenEmpty("+ tag", kDimmer);
            tagInput.setEscapeAndReturnKeysConsumed(false);
            tagInput.addKeyListener(this);
            tagInput.onReturnKey = [this]
            {
                const auto t = tagInput.getText().trim();
                if (t.isEmpty()) return;
                // Case-insensitive add matches the cloud-click and drag-drop
                // paths — keeps "Ambient" and "ambient" from coexisting.
                tags.addIfNotAlreadyThere(t, true);
                tagsTouched = true;
                tagInput.setText({}, false);
                refreshCloud();
                resized();
                repaint();
            };
            // Autocomplete: typing filters the suggestion cloud by prefix.
            tagInput.onTextChange = [this] { refreshCloud(); repaint(); };
            addAndMakeVisible(tagInput);

            cloud.onTagPicked = [this] (const juce::String& t)
            {
                tags.addIfNotAlreadyThere(t, true);
                tagsTouched = true;
                // Clear the filter so the next pick sees the full remainder.
                tagInput.setText({}, false);
                refreshCloud();
                resized();
                repaint();
            };
            addChildComponent(cloud);   // visibility decided in resized()

            configureBtn(saveBtn, kAccent);
            saveBtn.onClick = [this] { commit(); };
            addAndMakeVisible(saveBtn);

            configureBtn(cancelBtn, kSurface);
            cancelBtn.onClick = [this] { if (onCancelClicked) onCancelClicked(); };
            addAndMakeVisible(cancelBtn);

            configureBtn(copyBtn, kSurface);
            copyBtn.setButtonText("+ copy");
            copyBtn.onClick = [this]
            {
                const auto cur = nameEdit.getText().trim();
                if (cur.isEmpty()) return;
                if (! cur.endsWith(" copy"))
                    nameEdit.setText(cur + " copy", juce::sendNotificationSync);
            };
            addAndMakeVisible(copyBtn);
        }

        ~SaveDrawer() override
        {
            // Detach `this` as KeyListener from owned editors before they
            // start their own teardown — avoids a dangling-listener window
            // during member destruction.
            nameEdit.removeKeyListener(this);
            tagInput.removeKeyListener(this);
        }

        void configure(const juce::String& defaultName,
                       const juce::String& currentBank,
                       const juce::StringArray& existingBanks,
                       std::set<juce::String> pathKeys,
                       const std::vector<juce::String>& vocabulary,
                       const juce::String& promptAIn,
                       const juce::String& promptBIn,
                       bool canIncludeInferenceCacheIn)
        {
            cloud.setVocabulary(vocabulary);
            promptA       = promptAIn;
            promptB       = promptBIn;
            includeCacheAvailable = canIncludeInferenceCacheIn;
            includeCacheToggle.setToggleState(includeCacheAvailable, juce::dontSendNotification);
            includeCacheToggle.setVisible(false);
            includeCacheToggle.setEnabled(false);
            // Chips start empty and untouched; the owner pushes the save
            // target's tags via setTagsAuto() as soon as the conflict
            // state is computed (see refreshConflictUi() at the end of
            // this function).
            tags.clear();
            tagsTouched   = false;
            refreshCloud();
            existingPathKeys = std::move(pathKeys);

            // The auto-title heuristic only fires when the caller has no
            // meaningful name to offer (empty, default placeholder, or the
            // generic "New Preset" fallback from MainPanel). Real preset
            // names — current preset on Save, "X copy" on duplicate —
            // always win.
            const auto resolved = isPlaceholderName(defaultName)
                                      ? suggestTitleFromPrompts()
                                      : defaultName;
            nameEdit.setText(resolved, juce::dontSendNotification);

            bankBox.clear(juce::dontSendNotification);
            bankBox.addItem(kRootBankLabel(), 1);
            int nextId = 2;
            for (auto& b : existingBanks)
            {
                if (b.isEmpty() || b == kRootBankLabel()) continue;
                bankBox.addItem(b, nextId++);
            }
            const auto initial = currentBank.isEmpty() ? kRootBankLabel() : currentBank;
            bankBox.setText(initial, juce::dontSendNotification);

            refreshConflictUi();
        }

        void focusName()
        {
            nameEdit.grabKeyboardFocus();
            nameEdit.selectAll();
        }

        juce::String getName() const
        {
            return juce::File::createLegalFileName(nameEdit.getText().trim());
        }
        juce::StringArray getTags() const { return tags; }
        bool getIncludeInferenceCache() const
        {
            return includeCacheAvailable;
        }
        juce::String getBank() const
        {
            const auto raw = bankBox.getText().trim();
            if (raw.equalsIgnoreCase(kRootBankLabel())) return {};
            return juce::File::createLegalPathName(raw);
        }

        /** Returns the lowercased "bank/name.t5p" path key IFF the current
         *  (bank, name) tuple would overwrite an existing preset. Empty
         *  otherwise. Outer class uses this to highlight the matching list
         *  row in red. */
        juce::String getConflictPathKey() const
        {
            const auto name = getName();
            if (name.isEmpty()) return {};
            const auto pathKey = makePathKey(getBank(), name);
            return existingPathKeys.count(pathKey) > 0 ? pathKey : juce::String();
        }

        /** Programmatic bank selection — used when the Sidebar gets a bank
         *  click in Save mode. Empty string maps back to the user root. */
        void setBank(const juce::String& bank)
        {
            bankBox.setText(bank.isEmpty() ? kRootBankLabel() : bank,
                            juce::sendNotificationSync);
        }

        /** Point the drawer at an existing preset in ONE step — the save-mode
         *  list-row retarget. Both fields move together on purpose:
         *  setBank() + a separate name write would run refreshConflictUi()
         *  once on the mismatched (new bank, old name) tuple, and
         *  TextEditor::textChanged() only POSTS its change message, so the
         *  warning row and the Replace button would lag a message-loop turn
         *  behind the click. Silent setters + one explicit refresh instead:
         *  nameEdit.onTextChange and bankBox.onChange both do nothing but
         *  call refreshConflictUi(), so nothing is lost by suppressing them. */
        void retarget(const juce::String& name, const juce::String& bank)
        {
            const auto bankText = bank.isEmpty() ? kRootBankLabel() : bank;
            if (nameEdit.getText() == name && bankBox.getText() == bankText)
                return;

            bankBox.setText(bankText, juce::dontSendNotification);
            nameEdit.setText(name, juce::dontSendNotification);
            refreshConflictUi();
        }

        /** Owner-side auto-fill of the chip set from the would-be-
         *  overwritten file's on-disk tags (empty when the target is a
         *  new file). Ignored once the user has hand-edited the chips in
         *  this Save session — manual curation outlives target changes. */
        void setTagsAuto(juce::StringArray newTags)
        {
            if (tagsTouched) return;
            newTags.trim();
            newTags.removeEmptyStrings();
            newTags.removeDuplicates(true);
            if (newTags == tags) return;
            tags = std::move(newTags);
            refreshCloud();
            resized();
            repaint();
        }

        void beginNewBankEntry()
        {
            bankBox.setText({}, juce::sendNotificationSync);
            bankBox.grabKeyboardFocus();
            bankBox.showEditor();
        }

        // ── DragAndDropTarget ──
        // Source is the Detail-card chips: dropping a chip here adds the
        // tag to the drawer's tag set (idempotent — already-present tags
        // are dropped silently rather than duplicated).
        bool isInterestedInDragSource(const SourceDetails& d) override
        {
            return d.description.isString() && d.description.toString().isNotEmpty();
        }
        void itemDragEnter(const SourceDetails&) override
        {
            dropHover = true;
            repaint();
        }
        void itemDragExit(const SourceDetails&) override
        {
            dropHover = false;
            repaint();
        }
        void itemDropped(const SourceDetails& d) override
        {
            dropHover = false;
            const auto tag = d.description.toString().trim();
            // Case-insensitive uniqueness matches the invariant that
            // setTagsAuto() establishes via `removeDuplicates(true)`.
            if (tag.isNotEmpty())
            {
                tags.addIfNotAlreadyThere(tag, true);
                tagsTouched = true;
                refreshCloud();
                resized();
            }
            repaint();
        }

        /** How many rows the ACTIVE chips need at this width. Same measuring as
         *  the flow in paint() below - one place would be better, but the flow
         *  paints as it measures and cannot be asked without a Graphics. */
        int activeChipRowsNeeded(int width) const
        {
            int rows = 1, x = 0;
            for (const auto& t : tags)
            {
                const int chipW = juce::Font(juce::FontOptions(11.0f)).getStringWidth(t) + 28;
                if (x > 0 && x + chipW > width) { ++rows; x = 0; }
                x += chipW + 4;
            }
            return rows;
        }

        void paint(juce::Graphics& g) override
        {
            paintCard(g, getLocalBounds());

            // Drag-hover indicator: tints the chip area with the accent
            // colour while the user is hovering a tag-chip drag over it.
            if (dropHover && ! chipArea.isEmpty())
            {
                g.setColour(kAccent.withAlpha(0.14f));
                g.fillRoundedRectangle(chipArea.toFloat(), 4.0f);
                g.setColour(kAccent.withAlpha(0.55f));
                g.drawRoundedRectangle(chipArea.toFloat(), 4.0f, 1.5f);
            }

            // Tag chips fill whatever space chipArea got from resized().
            chipRects.clear();
            const int rowH = 22;
            const int gapX = 4;
            const int gapY = 4;
            int x = chipArea.getX();
            int y = chipArea.getY();
            for (int i = 0; ! chipArea.isEmpty() && i < tags.size(); ++i)
            {
                const auto& t = tags[i];
                const int chipW = juce::Font(juce::FontOptions(11.0f)).getStringWidth(t) + 28;
                if (x + chipW > chipArea.getRight())
                {
                    x = chipArea.getX();
                    y += rowH + gapY;
                    if (y + rowH > chipArea.getBottom()) break;
                }
                const auto chip = paintTagChip(g, x, y, t, ChipKind::ActiveRemovable);
                chipRects.push_back({ chip, i });
                x += chipW + gapX;
            }

            // The "Known tags" cloud below the active set is a child
            // component (see TagCloud) and paints itself into the bounds
            // resized() gave it.
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(12, 8);

            // Row 1: name (label + edit + + copy)
            auto row1 = area.removeFromTop(28);
            nameLabel.setBounds(row1.removeFromLeft(54));
            copyBtn.setBounds(row1.removeFromRight(60));
            row1.removeFromRight(6);
            nameEdit.setBounds(row1);

            // Inline conflict warning (consumes a fixed slot so the layout
            // doesn't jump on the first conflicting keystroke).
            warningLabel.setBounds(area.removeFromTop(14));
            area.removeFromTop(2);

            // Row 2: bank
            auto row2 = area.removeFromTop(26);
            bankLabel.setBounds(row2.removeFromLeft(54));
            bankBox.setBounds(row2);
            area.removeFromTop(6);

            if (includeCacheToggle.isVisible())
            {
                includeCacheToggle.setBounds(area.removeFromTop(22).withTrimmedLeft(54));
                area.removeFromTop(4);
            }

            // Action strip pinned to the bottom right.
            auto strip = area.removeFromBottom(30);
            saveBtn.setBounds(strip.removeFromRight(140));
            strip.removeFromRight(8);
            cancelBtn.setBounds(strip.removeFromRight(80));
            area.removeFromBottom(6);

            // Tags row: label + new-tag input on the right; chips fill the rest.
            auto tagsRow = area.removeFromTop(26);
            tagsLabel.setBounds(tagsRow.removeFromLeft(54));
            tagInput.setBounds(tagsRow.removeFromRight(120));
            area.removeFromTop(4);

            // Bottom slice = the "Known tags" cloud (header + scrolling chip
            // flow). Empty when there is no vocabulary at all, in which case
            // the active chip area absorbs the space. The slice no longer
            // decides which tags are reachable — the cloud scrolls.
            //
            // The split used to be a flat half, which reserved several empty
            // rows for the chips the preset does not have and squeezed the cloud
            // into two rows in a drawer with room for four or more. The active
            // set gets the rows it ACTUALLY needs (at least one, so there is
            // somewhere to drop a chip), and everything left over goes to the
            // cloud, up to six rows.
            const int chipsNeeded = activeChipRowsNeeded(area.getWidth());
            const int chipsH = juce::jmin(area.getHeight(), 22 + (chipsNeeded - 1) * 26);
            const int cloudH = cloud.hasVocabulary()
                                   ? juce::jlimit(TagCloud::minimumUsefulHeight(),
                                                  TagCloud::heightForRows(6),
                                                  area.getHeight() - chipsH - 4)
                                   : 0;
            cloud.setVisible(cloudH > 0);
            if (cloudH > 0)
                cloud.setBounds(area.removeFromBottom(cloudH));
            chipArea = area;
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            const auto p = e.getPosition();
            for (const auto& cr : chipRects)
            {
                const juce::Rectangle<int> closeBox(cr.bounds.getRight() - 16,
                                                    cr.bounds.getY(),
                                                    16, cr.bounds.getHeight());
                if (closeBox.contains(p))
                {
                    if (cr.index >= 0 && cr.index < tags.size())
                    {
                        tags.remove(cr.index);
                        tagsTouched = true;
                        refreshCloud();   // the removed tag is offerable again
                        resized();
                        repaint();
                    }
                    return;
                }
            }
            // Clicks on the "Known tags" cloud never reach here — it is its
            // own child component and reports picks via cloud.onTagPicked.
        }

    private:
        /** Push the two things that decide what the cloud may offer: the tags
         *  already staged for this save, and the input's prefix filter. */
        void refreshCloud() { cloud.update(tags, tagInput.getText()); }

        static const juce::String& kRootBankLabel()
        {
            return PresetManagerPanel::kRootUserBank();
        }

        static void configureLabel(juce::Label& l)
        {
            l.setColour(juce::Label::textColourId, kDim);
            l.setFont(juce::FontOptions(kUiLabelFontMin, juce::Font::bold));
        }
        static void configureEditor(juce::TextEditor& e)
        {
            e.setColour(juce::TextEditor::backgroundColourId, kSurface.brighter(0.04f));
            e.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            e.setColour(juce::TextEditor::outlineColourId, kBorder);
            e.setColour(juce::TextEditor::focusedOutlineColourId, kAccent);
            e.setSelectAllWhenFocused(true);
        }
        static void configureBtn(juce::TextButton& b, juce::Colour c)
        {
            b.setColour(juce::TextButton::buttonColourId, c);
            b.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        }

        void commit()
        {
            if (getName().isEmpty()) return;
            if (onSaveClicked) onSaveClicked();
        }

        void refreshConflictUi()
        {
            const auto name = getName();
            const auto bank = getBank();
            const auto pathKey = makePathKey(bank, name);
            const bool conflict = ! name.isEmpty() && existingPathKeys.count(pathKey) > 0;
            if (conflict)
            {
                const auto bankLabel = bank.isEmpty() ? kRootBankLabel() : bank;
                warningLabel.setText(juce::String::fromUTF8("\xe2\x9a\xa0  Will replace \"")
                                     + name + "\" in bank \"" + bankLabel + "\"",
                                     juce::dontSendNotification);
                warningLabel.setColour(juce::Label::textColourId, kWarningStrong);
                saveBtn.setButtonText("Replace \"" + name + "\"");
                saveBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffaa3333));
            }
            else
            {
                warningLabel.setText({}, juce::dontSendNotification);
                saveBtn.setButtonText("Save Preset");
                saveBtn.setColour(juce::TextButton::buttonColourId, kAccent);
            }

            if (onConflictChanged) onConflictChanged();
        }

        static juce::String makePathKey(const juce::String& bank, const juce::String& name)
        {
            if (name.isEmpty()) return {};
            const auto file = name + ".t5p";
            const auto key = bank.isEmpty() ? file : (bank + "/" + file);
            return key.toLowerCase();
        }

        bool keyPressed(const juce::KeyPress& key, juce::Component*) override
        {
            if (key == juce::KeyPress::escapeKey)
            {
                if (onCancelClicked) onCancelClicked();
                return true;
            }
            return false;
        }

        static bool isPlaceholderName(const juce::String& s) noexcept
        {
            const auto t = s.trim();
            return t.isEmpty()
                || t.equalsIgnoreCase("New Preset")
                || t.equalsIgnoreCase("T5ynth Export")
                || t.equalsIgnoreCase("Untitled");
        }

        /** Pulls a salient word from a free-text prompt: lowercased,
         *  ≥3 chars, not a common stop-word. Picks the LAST candidate so
         *  noun-phrase prompts like "warm distant thunder" yield the
         *  head noun ("thunder") rather than the leading adjective. */
        static juce::String pickSalientWord(const juce::String& src)
        {
            static const juce::StringArray stop {
                "the","and","with","into","over","under","from","this","that",
                "very","quite","more","less","some","any","for","you","not",
                "but","are","was","were","has","had","its","their","them",
                "like","just","only","also","than","then","there","what",
                "when","while","which","who","why","how"
            };
            auto words = juce::StringArray::fromTokens(src,
                " \t\n\r,.;:!?\"'/\\()[]{}_<>", {});
            for (int i = words.size(); --i >= 0; )
            {
                const auto w = words[i].toLowerCase().trim();
                if (w.length() < 3 || stop.contains(w)) continue;
                return w;
            }
            return {};
        }

        /** Builds a save-name suggestion from prompts A and B. Empty if
         *  both prompts are empty or yield no salient word. */
        juce::String suggestTitleFromPrompts() const
        {
            auto cap = [](juce::String s) -> juce::String
            {
                return s.isEmpty() ? s
                                   : s.substring(0, 1).toUpperCase() + s.substring(1);
            };
            const auto a = pickSalientWord(promptA);
            const auto b = pickSalientWord(promptB);
            if (a.isNotEmpty() && b.isNotEmpty() && a != b) return cap(a) + "-" + cap(b);
            if (a.isNotEmpty()) return cap(a);
            if (b.isNotEmpty()) return cap(b);
            return "New Preset";
        }

        juce::Label      nameLabel, bankLabel, tagsLabel, warningLabel;
        juce::TextEditor nameEdit, tagInput;
        juce::ComboBox   bankBox;
        juce::ToggleButton includeCacheToggle;
        juce::TextButton saveBtn   { "Save Preset" };
        juce::TextButton cancelBtn { "Cancel" };
        juce::TextButton copyBtn;
        juce::StringArray         tags;
        // True once the user has manually added/removed a chip in this
        // Save session — from then on setTagsAuto() is a no-op so target
        // retargeting (name edits, row clicks) can't clobber hand edits.
        bool                      tagsTouched = false;
        std::set<juce::String>    existingPathKeys;
        juce::String              promptA, promptB;
        bool                      includeCacheAvailable = false;

        struct ChipRect { juce::Rectangle<int> bounds; int index; };
        std::vector<ChipRect> chipRects;        // active tags (× to remove)
        juce::Rectangle<int>  chipArea;
        bool                  dropHover = false;
        TagCloud              cloud;
    };
    SaveDrawer saveDrawer;

    // ─── ListBoxModel ────────────────────────────────────────────────────
    int getNumRows() override { return (int) filteredIndices.size(); }

    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (! juce::isPositiveAndBelow(rowNumber, (int) filteredIndices.size())) return;
        const int entryIdx = filteredIndices[(size_t) rowNumber];
        const auto& e = allEntries[(size_t) entryIdx];

        // In Save mode the would-replace row gets a red wash that takes
        // visual precedence over selection blue and the current-preset
        // edge marker — the user's attention should be on the destructive
        // outcome, not on the bookkeeping highlights.
        const bool isConflictRow = (currentMode == Mode::Save
                                    && entryIdx == conflictEntryIndex);

        if (isConflictRow)
        {
            g.setColour(kWarningStrong.withAlpha(0.28f));
            g.fillRect(0, 0, width, height);
            g.setColour(kWarningStrong);
            g.fillRect(0, 0, 3, height);
        }
        else
        {
            if (rowIsSelected)
            {
                g.setColour(kAccent.withAlpha(0.22f));
                g.fillRect(0, 0, width, height);
            }
            if (e.file == currentFile)
            {
                g.setColour(kSeqCol);
                g.fillRect(0, 0, 3, height);
            }
        }
        g.setColour(kBorder.withAlpha(0.35f));
        g.drawLine(0.0f, (float) height - 1.0f, (float) width, (float) height - 1.0f, 0.5f);

        auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(10, 6);
        auto topRow = bounds.removeFromTop(20);
        // Reserve at most half the row width for the date column, and only if
        // the row has room for a meaningful name first.
        const int dateW = topRow.getWidth() > 180
                              ? juce::jmin(90, topRow.getWidth() / 2)
                              : 0;
        auto dateArea = topRow.removeFromRight(dateW);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(13.5f));
        g.drawText(e.name, topRow, juce::Justification::centredLeft, true);
        if (dateW > 0)
        {
            g.setColour(kDimmer);
            g.setFont(juce::FontOptions(kUiLabelFontMin));
            g.drawText(e.modified.formatted("%Y-%m-%d"), dateArea, juce::Justification::centredRight, false);
        }

        bounds.removeFromTop(2);
        g.setColour(kDim);
        g.setFont(juce::FontOptions(11.5f));
        const auto snip = snippet(e.displayPrompt(), 96);
        g.drawText(snip.isEmpty() ? juce::String::fromUTF8("\xe2\x80\x94") : snip,
                   bounds.removeFromTop(14), juce::Justification::centredLeft, true);
    }

    /** Point the Save-Drawer at an existing preset: the user is saying
     *  "save under THIS name (overwriting that file)". The cross-bank gate
     *  and the Replace-confirm dialog still apply at commit time.
     *
     *  SaveDrawer::retarget() writes an unchanged (name, bank) tuple without
     *  a notification, so calling this twice for one gesture (see
     *  listBoxItemClicked) is a no-op the second time. */
    void retargetSaveTo(int entryIndex)
    {
        if (currentMode != Mode::Save) return;
        if (! juce::isPositiveAndBelow(entryIndex, (int) allEntries.size())) return;

        const auto& entry = allEntries[(size_t) entryIndex];
        saveDrawer.retarget(entry.name,
                            entry.bank == kRootUserBank() ? juce::String()
                                                          : entry.bank);
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        if (! juce::isPositiveAndBelow(lastRowSelected, (int) filteredIndices.size()))
        {
            selectedEntryIndex = -1;
            detail.clear();
            return;
        }
        selectedEntryIndex = filteredIndices[(size_t) lastRowSelected];
        detail.setEntry(allEntries[(size_t) selectedEntryIndex]);

        // Save-mode retarget on the keyboard-arrow path (mouse clicks are
        // handled in listBoxItemClicked, which also covers clicks on the
        // already-selected row -- those never reach this callback).
        //
        // Skipped when the selection change was triggered programmatically
        // (search-box typing, sidebar filter clicks, bank creation,
        // currentPreset restoration -- see the `programmaticSelection`
        // guard at each presetList.selectRow callsite). Without that
        // guard, those auto-selects would clobber the typed Save name.
        if (! programmaticSelection)
            retargetSaveTo(selectedEntryIndex);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        // Loading is suppressed during Save mode — the user is committing a
        // new file, not navigating away from the current state.
        if (currentMode == Mode::Save) return;
        if (! juce::isPositiveAndBelow(row, (int) filteredIndices.size())) return;
        if (onLoadRequested) onLoadRequested(allEntries[(size_t) filteredIndices[(size_t) row]].file);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow(row, (int) filteredIndices.size())) return;
        const int entryIndex = filteredIndices[(size_t) row];

        if (e.mods.isPopupMenu())
        {
            if (currentMode == Mode::Save) return;  // rename/delete suppressed in Save mode
            presetList.selectRow(row, false, true);
            showRowContextMenu(entryIndex, e.getScreenPosition());
            return;
        }

        // Left-click in Save mode retargets HERE, not via selectedRowsChanged.
        // ListBox::selectRowInternal early-outs on a row that is already
        // selected, so clicking the row the panel auto-selected (the row-0
        // fallback after a search, or the currently-loaded preset) produces
        // NO selection change and the save target silently stayed on the
        // previous name. That is the most common path through this dialog:
        // type part of a name, click the one row that survives the filter.
        //
        // ListBox resolves the selection (selectRowsBasedOnModifierKeys)
        // before calling this, so on a genuinely new row selectedRowsChanged
        // has already retargeted and this repeats it harmlessly.
        if (currentMode == Mode::Save)
            retargetSaveTo(entryIndex);
    }

    void backgroundClicked(const juce::MouseEvent&) override
    {
        // No-op; required by ListBoxModel signature compatibility.
    }

    void showRowContextMenu(int entryIndex, juce::Point<int> screenPos)
    {
        if (! juce::isPositiveAndBelow(entryIndex, (int) allEntries.size())) return;
        const auto& entry = allEntries[(size_t) entryIndex];
        const auto file = entry.file;

        juce::PopupMenu menu;
        menu.addItem(1, juce::String::fromUTF8("Rename\xe2\x80\xa6"), true);
        menu.addItem(3, "Duplicate",                                  true);
       #if JUCE_MAC
        menu.addItem(4, "Show in Finder", true);
       #elif JUCE_WINDOWS
        menu.addItem(4, "Show in Explorer", true);
       #else
        menu.addItem(4, "Show in File Manager", true);
       #endif
        menu.addSeparator();
        menu.addItem(2, "Delete",                                     true);

        // Anchor at the cursor (1×1 px screen-area) instead of the
        // listbox bounds; otherwise the popup snaps to the panel's
        // bottom edge and looks broken.
        const juce::Rectangle<int> targetArea(screenPos.x, screenPos.y, 1, 1);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(targetArea),
            [this, file](int result)
            {
                switch (result)
                {
                    case 1: if (onRenameRequested)    onRenameRequested(file);    break;
                    case 2: if (onDeleteRequested)    onDeleteRequested(file);    break;
                    case 3: if (onDuplicateRequested) onDuplicateRequested(file); break;
                    case 4: if (onRevealRequested)    onRevealRequested(file);    break;
                    default: break;
                }
            });
    }

    // ─── KeyListener ─────────────────────────────────────────────────────
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            // Esc always closes the entire overlay — same as the bottom
            // Close button — regardless of mode. The user came to either
            // load or save; cancelling means abort all the way back to
            // the synth, not switch to a different overlay mode.
            if (onCloseRequested) onCloseRequested();
            return true;
        }
        return false;
    }

    // ─── State ───────────────────────────────────────────────────────────
    juce::Label titleLabel;
    juce::TextEditor searchEditor;
    juce::TextButton importBtn { "Import Presets" };
    juce::TextButton githubBtn { "Update Library" };
    juce::TextButton closeIconBtn;     // top-right × icon
    juce::TextButton cancelBtn { "Close" };
    juce::Label statusLabel;
    juce::ListBox presetList;

    std::vector<Entry> allEntries;
    std::vector<int>   filteredIndices;
    int selectedEntryIndex = -1;
    juce::File   currentFile;
    juce::String currentName;
    Mode         currentMode = Mode::Browse;
    int          conflictEntryIndex = -1;   // -1 = no would-replace target
    bool         updaterBusy = false;       // true → Update button suppressed
    // Set true around programmatic presetList.selectRow() calls so the
    // selectedRowsChanged callback can distinguish "user picked this row"
    // (mouse click OR keyboard arrow) from "we just auto-restored the
    // previous selection after a rebuild". The save-mode retarget logic
    // only fires on the user-initiated path -- otherwise typing in the
    // search box or clicking a sidebar filter (which auto-selects row 0)
    // would silently clobber the typed Save name.
    bool         programmaticSelection = false;

    // Canonical tag taxonomy injected by MainPanel via
    // setTagVocabularyCanonical(). Merged with sidebar-seen user tags into
    // the cloud rendered by Detail + SaveDrawer.
    juce::StringArray         canonicalTagVocabulary;
    std::vector<juce::String> mergedTagVocabulary;   // canonical ∪ user-tags

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManagerPanel)
};
