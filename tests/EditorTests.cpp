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
