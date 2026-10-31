#include "feedback_options_qt.h"
#include "decoder_application_controller.h"
#include "decoder_progress_text.h"
#include "unified_decoder_test_support.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <catch2/catch_test_macros.hpp>

class DecoderApplicationControllerTestAccess
{
public:
    static void Isolate(DecoderApplicationController& controller, const QString& root)
    {
        controller.pollTimer_.stop();
        controller.log_ = pbgui::OperationalLog(QStringLiteral("Decoder"), root);
    }
    static void Poll(DecoderApplicationController& controller)
    {
        controller.PollSnapshot();
    }
};

namespace
{
QApplication& Application()
{
    static int count = 1;
    static char name[] = "feedback-options-test";
    static char* arguments[] = {name, nullptr};
    static QApplication application(count, arguments);
    return application;
}

QString ScratchTemplate()
{
    return QString::fromStdWString(std::filesystem::path(PB_TEST_SCRATCH_ROOT).wstring()) + QStringLiteral("/feedback-options-XXXXXX");
}
}

TEST_CASE("Feedback options are off by default and share Encoder main Shell settings without path I/O", "[feedback][gui]")
{
    static_cast<void>(Application());
    QTemporaryDir scratch(ScratchTemplate());
    REQUIRE(scratch.isValid());
    QSettings settings(scratch.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    pbgui::FeedbackOptions main(pbgui::FeedbackRole::EncoderReader);
    main.Load(settings);
    auto* const enabled = main.findChild<QCheckBox*>(QStringLiteral("feedbackEnabled"));
    auto* const path = main.findChild<QLineEdit*>(QStringLiteral("feedbackPath"));
    auto* const kind = main.findChild<QComboBox*>(QStringLiteral("feedbackLocationKind"));
    REQUIRE(enabled);
    REQUIRE(path);
    REQUIRE(kind);
    REQUIRE_FALSE(enabled->isChecked());
    pbapp::EncoderConfig config;
    REQUIRE(main.Apply(config).isEmpty());
    REQUIRE_FALSE(config.feedback);
    enabled->setChecked(true);
    REQUIRE_FALSE(main.Apply(config).isEmpty());
    path->setText(QStringLiteral("\\\\Client\\C$\\Feedback Status"));
    for (const auto profile : {pbapp::VisualProfile::UnifiedLc4, pbapp::VisualProfile::UnifiedGrayFast,
        pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide})
    {
        config.visualProfile = profile;
        REQUIRE(main.Apply(config).isEmpty());
        REQUIRE(config.feedback);
        REQUIRE(config.feedback->path == LR"(\\Client\C$\Feedback Status)");
        REQUIRE(config.feedback->locationKind == pbapp::FeedbackLocationKind::Directory);
    }
    kind->setCurrentIndex(1);
    path->setText(QStringLiteral("\\\\Client\\C$\\反馈 测试\\chosen.pbfb"));
    main.Save(settings);
    pbgui::FeedbackOptions shell(pbgui::FeedbackRole::EncoderReader);
    shell.Load(settings);
    REQUIRE(shell.Apply(config).isEmpty());
    REQUIRE(config.feedback->path == LR"(\\Client\C$\反馈 测试\chosen.pbfb)");
    REQUIRE(config.feedback->locationKind == pbapp::FeedbackLocationKind::File);
    shell.SetEditable(false);
    REQUIRE_FALSE(shell.findChild<QCheckBox*>(QStringLiteral("feedbackEnabled"))->isEnabled());
    REQUIRE_FALSE(shell.findChild<QLineEdit*>(QStringLiteral("feedbackPath"))->isEnabled());
    shell.SetEditable(true);
    REQUIRE(shell.findChild<QLineEdit*>(QStringLiteral("feedbackPath"))->isEnabled());
    pbgui::FeedbackOptions decoder(pbgui::FeedbackRole::DecoderWriter);
    decoder.Load(settings);
    pbapp::DecoderConfig decoderConfig;
    REQUIRE(decoder.Apply(decoderConfig).isEmpty());
    REQUIRE_FALSE(decoderConfig.feedback);
}

TEST_CASE("Malformed feedback preferences fail closed while disabled invalid text is ignored", "[feedback][gui]")
{
    static_cast<void>(Application());
    QTemporaryDir scratch(ScratchTemplate());
    REQUIRE(scratch.isValid());
    QSettings settings(scratch.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("feedback/encoder/enabled"), QStringLiteral("broken"));
    settings.setValue(QStringLiteral("feedback/encoder/locationKind"), 7);
    pbgui::FeedbackOptions options(pbgui::FeedbackRole::EncoderReader);
    options.Load(settings);
    auto* const enabled = options.findChild<QCheckBox*>(QStringLiteral("feedbackEnabled"));
    auto* const path = options.findChild<QLineEdit*>(QStringLiteral("feedbackPath"));
    REQUIRE_FALSE(enabled->isChecked());
    path->setText(QStringLiteral("relative-invalid"));
    pbapp::EncoderConfig config;
    REQUIRE(options.Apply(config).isEmpty());
    REQUIRE_FALSE(config.feedback);
    enabled->setChecked(true);
    REQUIRE_FALSE(options.Apply(config).isEmpty());
    path->setText(scratch.filePath(QStringLiteral("new dedicated.pbfb")));
    options.findChild<QComboBox*>(QStringLiteral("feedbackLocationKind"))->setCurrentIndex(1);
    REQUIRE(options.Apply(config).isEmpty());
    REQUIRE(config.feedback);
    REQUIRE_FALSE(std::filesystem::exists(config.feedback->path));
}

TEST_CASE("Feedback options remain wrapped and scroll reachable at small and resized DPI layouts", "[feedback][gui][layout]")
{
    auto& application = Application();
    QScrollArea scroll;
    scroll.setWidgetResizable(true);
    auto* const options = new pbgui::FeedbackOptions(pbgui::FeedbackRole::DecoderWriter);
    options->findChild<QCheckBox*>(QStringLiteral("feedbackEnabled"))->setChecked(true);
    scroll.setWidget(options);
    for (const int width : {360, 520, 760})
    {
        scroll.resize(width, 150);
        scroll.show();
        application.processEvents();
        auto* const hint = options->findChild<QLabel*>(QStringLiteral("feedbackHint"));
        REQUIRE(hint);
        REQUIRE(hint->wordWrap());
        REQUIRE(hint->height() >= hint->heightForWidth(hint->width()));
        scroll.ensureWidgetVisible(hint);
        application.processEvents();
        REQUIRE(scroll.viewport()->rect().intersects(QRect(hint->mapTo(scroll.viewport(), QPoint()), hint->size())));
    }
}

TEST_CASE("Decoder controller keeps verified completion listening busy until explicit stop", "[feedback][gui][lifecycle]")
{
    static_cast<void>(Application());
    QTemporaryDir scratch(ScratchTemplate());
    REQUIRE(scratch.isValid());
    const auto root = std::filesystem::path(scratch.path().toStdWString());
    const auto frames = g16test::MakeFrames(root / L"source", g16test::RawBytes(32));
    const auto output = root / L"output";
    const auto feedback = root / L"feedback";
    REQUIRE(std::filesystem::create_directory(output));
    REQUIRE(std::filesystem::create_directory(feedback));
    const auto state = std::make_shared<g16test::ReceiveState>();
    DecoderApplicationController controller(nullptr, g16test::Services(state));
    DecoderApplicationControllerTestAccess::Isolate(controller, scratch.filePath(QStringLiteral("logs")));
    auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), g16test::Region());
    config.feedback = pbapp::FeedbackConfig{feedback};
    REQUIRE(controller.Start(config).isEmpty());
    state->Push(frames.front());
    REQUIRE(g16test::WaitFor([&]() { return controller.GetSnapshot().state == pbapp::DecoderState::Completed; }));
    const auto completed = controller.GetSnapshot();
    REQUIRE(controller.IsActive());
    REQUIRE(pbgui::IsVerifiedCompletion(completed));
    DecoderApplicationControllerTestAccess::Poll(controller);
    REQUIRE(controller.GetSnapshot().feedback.listening);
    REQUIRE(controller.IsActive());
    const auto elapsed = pbgui::FormatDecoderProgress(completed, 1).elapsed;
    REQUIRE(pbgui::FormatDecoderProgress(controller.GetSnapshot(), 9'999'999).elapsed == elapsed);
    controller.RequestStop();
    REQUIRE(g16test::WaitFor([&]() { return !controller.GetSnapshot().feedback.listening; }));
    DecoderApplicationControllerTestAccess::Poll(controller);
    REQUIRE_FALSE(controller.IsActive());
    REQUIRE(pbgui::IsVerifiedCompletion(controller.StopAndGetSnapshot()));
    REQUIRE(controller.Start(config).isEmpty());
    state->Push(frames.front());
    REQUIRE(g16test::WaitFor([&]() { return controller.GetSnapshot().state == pbapp::DecoderState::Completed; }));
    REQUIRE(pbgui::IsVerifiedCompletion(controller.StopAndGetSnapshot()));
}
