#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

#include "DelaySettings.h"
#include "ParameterIds.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace ids = pitchdelay::ids;

namespace
{
struct EditorFixture
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PitchDelayProcessor proc;
    std::unique_ptr<PitchDelayEditor> editor;

    EditorFixture()
    {
        proc.setPlayConfigDetails (2, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        editor = std::make_unique<PitchDelayEditor> (proc);
    }

    void typeSamples (const juce::String& text)
    {
        editor->getSamplesField().getLabel().setText (text, juce::sendNotificationSync);
    }
    void typeMilliseconds (const juce::String& text)
    {
        editor->getMillisecondsField().getLabel().setText (text, juce::sendNotificationSync);
    }
};
}

TEST_CASE ("the fields show the delay in samples and milliseconds, linked")
{
    EditorFixture f;
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "43200");
    REQUIRE (f.editor->getMillisecondsField().getLabel().getText() == "900.000");

    f.proc.setDelayFrames (43210);
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "43210");
    REQUIRE (f.editor->getMillisecondsField().getLabel().getText() == "900.208");
}

TEST_CASE ("typing samples sets the delay exactly")
{
    EditorFixture f;
    f.typeSamples ("43210");
    REQUIRE (f.proc.getCurrentDelayFrames() == 43210);
    f.typeSamples ("  7 ");
    REQUIRE (f.proc.getCurrentDelayFrames() == 7);
    f.typeSamples ("0043210");
    REQUIRE (f.proc.getCurrentDelayFrames() == 43210);
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "43210");   // canonical text restored
}

TEST_CASE ("typing milliseconds rounds to the nearest frame")
{
    EditorFixture f;
    f.typeMilliseconds ("900.5");
    REQUIRE (f.proc.getCurrentDelayFrames() == 43224);
    f.typeMilliseconds ("1000");
    REQUIRE (f.proc.getCurrentDelayFrames() == 48000);
    f.typeMilliseconds ("0,5");              // decimal comma
    REQUIRE (f.proc.getCurrentDelayFrames() == 24);
    f.typeMilliseconds ("0.0104");           // 0.4992 frames -> 0
    REQUIRE (f.proc.getCurrentDelayFrames() == 0);
}

TEST_CASE ("garbage typed into a field leaves the delay unchanged")
{
    EditorFixture f;
    f.proc.setDelayFrames (5000);
    for (const char* bad : { "abc", "", "   ", "-5", "12x", "1.5", "1e3", "--" })
    {
        INFO ("samples text '" << bad << "'");
        f.typeSamples (bad);
        REQUIRE (f.proc.getCurrentDelayFrames() == 5000);
    }
    for (const char* bad : { "abc", "", "-5", "1.2.3", "12x", "1e3" })
    {
        INFO ("ms text '" << bad << "'");
        f.typeMilliseconds (bad);
        REQUIRE (f.proc.getCurrentDelayFrames() == 5000);
    }
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "5000");   // display restored
}

TEST_CASE ("huge numbers clamp to the maximum delay instead of overflowing")
{
    EditorFixture f;
    f.typeSamples ("99999999999999999999");
    REQUIRE (f.proc.getCurrentDelayFrames() == 480000);
    f.typeMilliseconds ("99999999999999999999");
    REQUIRE (f.proc.getCurrentDelayFrames() == 480000);
}

TEST_CASE ("the sample buttons step by one frame and stop at zero")
{
    EditorFixture f;
    f.proc.setDelayFrames (100);
    f.editor->getSamplesField().getUpButton().onClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 101);
    f.editor->getSamplesField().getDownButton().onClick();
    f.editor->getSamplesField().getDownButton().onClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 99);

    f.proc.setDelayFrames (0);
    f.editor->getSamplesField().getDownButton().onClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 0);
}

TEST_CASE ("the millisecond buttons step by one millisecond")
{
    EditorFixture f;                       // 43200 frames = 900 ms at 48 kHz
    f.editor->getMillisecondsField().getUpButton().onClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 43248);
    f.editor->getMillisecondsField().getDownButton().onClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 43200);

    f.proc.setDelayFrames (10);            // less than 1 ms: down clamps to 0
    f.editor->getMillisecondsField().getDownButton().onClick();
    REQUIRE (f.proc.getCurrentDelayFrames() == 0);
}

TEST_CASE ("changing the speed through the parameter updates the fields")
{
    EditorFixture f;
    auto* p = f.proc.apvts.getParameter (ids::mode);
    p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getSamplesField().getLabel().getText() == "32000");
    REQUIRE (f.editor->getMillisecondsField().getLabel().getText() == "666.667");
}

TEST_CASE ("the readout shows the delay and the calibration status")
{
    EditorFixture f;
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getReadoutText() == "Delay: 43200 frames = 900.000 ms @ 48.0 kHz");
    REQUIRE (f.editor->getStatusText().isEmpty());

    auto* p = f.proc.apvts.getParameter (ids::calibration);
    p->setValueNotifyingHost (1.0f);
    f.editor->updateFromProcessor();
    REQUIRE (f.editor->getStatusText().contains ("CALIBRATION"));
    REQUIRE (f.editor->getStatusText().contains ("43200"));
}

TEST_CASE ("the editor paints its own opaque background so the light label text is readable")
{
    EditorFixture f;
    const auto image = f.editor->createComponentSnapshot (f.editor->getLocalBounds(), true, 1.0f);
    const auto expected = f.editor->getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    const auto pixel = image.getPixelAt (3, 3);   // top-left corner: nothing is drawn there
    REQUIRE (pixel.getARGB() == expected.getARGB());
}

// ---- keys belong to the host's transport ------------------------------------------

namespace
{
void collectDescendants (juce::Component& c, std::vector<juce::Component*>& out)
{
    out.push_back (&c);
    for (auto* child : c.getChildren())
        collectDescendants (*child, out);
}

juce::KeyPress keyOf (juce::juce_wchar c, juce::ModifierKeys mods = {}) { return juce::KeyPress ((int) c, mods, c); }
}

TEST_CASE ("no control of the editor takes keyboard focus, so Space and Return reach the host")
{
    EditorFixture f;
    std::vector<juce::Component*> all;
    collectDescendants (*f.editor, all);
    REQUIRE (all.size() > 10);   // the traversal really found the controls
    for (auto* c : all)
    {
        INFO ("component '" << c->getName() << "' (" << typeid (*c).name() << ")");
        REQUIRE_FALSE (c->getWantsKeyboardFocus());
        REQUIRE_FALSE (c->getMouseClickGrabsKeyboardFocus());
    }
}

TEST_CASE ("an active number field uses only the keys needed for typing a number")
{
    NumberEntryEditor ed;
    ed.setText ("123", false);

    // keys that edit or confirm the number
    for (auto key : { keyOf ('0'), keyOf ('7'), keyOf ('9'), keyOf ('.'), keyOf (','),
                      juce::KeyPress (juce::KeyPress::backspaceKey), juce::KeyPress (juce::KeyPress::deleteKey),
                      juce::KeyPress (juce::KeyPress::leftKey), juce::KeyPress (juce::KeyPress::rightKey),
                      juce::KeyPress (juce::KeyPress::homeKey), juce::KeyPress (juce::KeyPress::endKey),
                      juce::KeyPress (juce::KeyPress::returnKey), juce::KeyPress (juce::KeyPress::escapeKey),
                      keyOf ('a', juce::ModifierKeys::commandModifier), keyOf ('c', juce::ModifierKeys::commandModifier),
                      keyOf ('v', juce::ModifierKeys::commandModifier), keyOf ('x', juce::ModifierKeys::commandModifier),
                      keyOf ('z', juce::ModifierKeys::commandModifier) })
    {
        INFO ("key code " << key.getKeyCode() << " char " << (int) key.getTextCharacter());
        REQUIRE (ed.keyPressed (key));
    }
}

TEST_CASE ("an active number field hands every other key to the host")
{
    NumberEntryEditor ed;
    ed.setText ("123", false);

    for (auto key : { keyOf (' '), keyOf ('a'), keyOf ('r'), keyOf ('c'), keyOf ('k'), keyOf ('-'), keyOf ('+'),
                      keyOf ('*'), keyOf ('/'), keyOf ('['), keyOf (']'),
                      juce::KeyPress (juce::KeyPress::upKey), juce::KeyPress (juce::KeyPress::downKey),
                      juce::KeyPress (juce::KeyPress::F1Key), juce::KeyPress (juce::KeyPress::F5Key),
                      keyOf ('s', juce::ModifierKeys::commandModifier),
                      keyOf ('q', juce::ModifierKeys::commandModifier),
                      keyOf ('5', juce::ModifierKeys::commandModifier),
                      keyOf (' ', juce::ModifierKeys::shiftModifier),
                      keyOf ('5', juce::ModifierKeys::altModifier) })
    {
        INFO ("key code " << key.getKeyCode() << " char " << (int) key.getTextCharacter()
              << " mods " << key.getModifiers().getRawFlags());
        REQUIRE_FALSE (ed.keyPressed (key));
    }
    REQUIRE (ed.getText() == "123");   // none of them changed the text
}

TEST_CASE ("typing into an active number field edits the number and nothing else")
{
    NumberEntryEditor ed;
    ed.setText ("", false);
    for (auto c : juce::String ("4 3a2.5-"))
        ed.keyPressed (keyOf (c));
    REQUIRE (ed.getText() == "432.5");   // space, letters and '-' were handed on, not inserted
}

TEST_CASE ("the number fields create the key-filtering editor and only accept number characters")
{
    EditorFixture f;
    auto& label = f.editor->getSamplesField().getLabel();
    label.showEditor();
    auto* ed = dynamic_cast<NumberEntryEditor*> (label.getCurrentTextEditor());
    REQUIRE (ed != nullptr);
    ed->setText ("", false);
    ed->insertTextAtCaret ("a5 b,6.x");           // pasted text is filtered as well
    REQUIRE (ed->getText() == "5,6.");
    label.hideEditor (true);
}

// Optional: renders the editor to PNG files in $PITCHDELAY_SNAPSHOT_DIR for a visual check
// without opening a window. Does nothing when the variable is not set.
TEST_CASE ("editor snapshot (only when PITCHDELAY_SNAPSHOT_DIR is set)")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("PITCHDELAY_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
        return;

    EditorFixture f;
    const auto save = [&f, &dir] (const juce::String& name)
    {
        f.editor->updateFromProcessor();
        const auto image = f.editor->createComponentSnapshot (f.editor->getLocalBounds(), true, 2.0f);
        juce::File file (dir);
        file = file.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream out (file);
        REQUIRE (out.openedOk());
        juce::PNGImageFormat().writeImageToStream (image, out);
    };
    save ("editor-default.png");

    auto* calibration = f.proc.apvts.getParameter (ids::calibration);
    calibration->setValueNotifyingHost (1.0f);
    f.proc.setDelayFrames (43210);
    save ("editor-calibration.png");
}
