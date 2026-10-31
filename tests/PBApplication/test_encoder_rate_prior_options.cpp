#include "encoder_rate_prior_options.h"

#include <QApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

namespace
{

QApplication& GuiApplication()
{
    static int argumentCount = 1;
    static char applicationName[] = "rate-prior-options-test";
    static char* arguments[] = {applicationName, nullptr};
    static QApplication application(argumentCount, arguments);
    return application;
}

}

TEST_CASE("Both Encoder entries share opt-in settings with explicit units and PAM4-only application", "[rate-prior][gui]")
{
    static_cast<void>(GuiApplication());
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    QSettings settings(scratch.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    pbencoder::EncoderRatePriorOptions mainEntry;
    mainEntry.Load(settings);
    auto* const enabled = mainEntry.findChild<QCheckBox*>(QStringLiteral("expectedReceptionRateEnabled"));
    auto* const minimum = mainEntry.findChild<QSpinBox*>(QStringLiteral("expectedReceptionRateMinimum"));
    auto* const maximum = mainEntry.findChild<QSpinBox*>(QStringLiteral("expectedReceptionRateMaximum"));
    REQUIRE(enabled);
    REQUIRE(minimum);
    REQUIRE(maximum);
    REQUIRE_FALSE(enabled->isChecked());
    mainEntry.SetContext(pbapp::VisualProfile::ExperimentalPam4, true);
    REQUIRE_FALSE(minimum->isEnabled());
    enabled->setChecked(true);
    minimum->setValue(200);
    maximum->setValue(280);
    REQUIRE(minimum->isEnabled());
    pbapp::EncoderConfig config;
    config.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    REQUIRE(mainEntry.Apply(config).isEmpty());
    REQUIRE(config.expectedReceptionRate == pbapp::SenderRatePrior{200, 280});
    mainEntry.Save(settings);
    pbencoder::EncoderRatePriorOptions shellEntry;
    shellEntry.Load(settings);
    REQUIRE(shellEntry.Apply(config).isEmpty());
    REQUIRE(config.expectedReceptionRate == pbapp::SenderRatePrior{200, 280});
    shellEntry.SetContext(pbapp::VisualProfile::ExperimentalPam4Wide, true);
    REQUIRE_FALSE(shellEntry.findChild<QCheckBox*>()->isEnabled());
    config.visualProfile = pbapp::VisualProfile::ExperimentalPam4Wide;
    REQUIRE(shellEntry.Apply(config).isEmpty());
    REQUIRE_FALSE(config.expectedReceptionRate);
    shellEntry.SetContext(pbapp::VisualProfile::ExperimentalPam4, false);
    REQUIRE_FALSE(shellEntry.findChild<QSpinBox*>(QStringLiteral("expectedReceptionRateMinimum"))->isEnabled());
    shellEntry.SetContext(pbapp::VisualProfile::ExperimentalPam4, true);
    REQUIRE(shellEntry.findChild<QSpinBox*>(QStringLiteral("expectedReceptionRateMinimum"))->isEnabled());
}

TEST_CASE("Enabled reversed prior is rejected and malformed saved settings cannot silently enable it", "[rate-prior][gui]")
{
    static_cast<void>(GuiApplication());
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    QSettings settings(scratch.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    pbencoder::EncoderRatePriorOptions entry;
    entry.SetContext(pbapp::VisualProfile::ExperimentalPam4, true);
    entry.findChild<QCheckBox*>()->setChecked(true);
    entry.findChild<QSpinBox*>(QStringLiteral("expectedReceptionRateMinimum"))->setValue(281);
    pbapp::EncoderConfig config;
    config.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    REQUIRE_FALSE(entry.Apply(config).isEmpty());
    REQUIRE_FALSE(config.expectedReceptionRate);
    settings.setValue(QStringLiteral("encoder/pam4RatePriorEnabled"), true);
    settings.setValue(QStringLiteral("encoder/pam4RatePriorMinimumKibPerSecond"), QStringLiteral("not-a-number"));
    settings.setValue(QStringLiteral("encoder/pam4RatePriorMaximumKibPerSecond"), 280);
    entry.Load(settings);
    REQUIRE_FALSE(entry.findChild<QCheckBox*>()->isChecked());
    REQUIRE(entry.findChild<QLabel*>(QStringLiteral("expectedReceptionRateHint"))->text().contains(QStringLiteral("无效")));
    REQUIRE(entry.Apply(config).isEmpty());
    REQUIRE_FALSE(config.expectedReceptionRate);
}
