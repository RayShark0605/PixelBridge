#pragma once

#include "local_desktop_runtime.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace pbencoder
{

// One UI/settings contract shared by the normal and Shell entry points.
// Deliberately independent of the rendering backend and of receiver state.
class EncoderRatePriorOptions final : public QWidget
{
public:
    explicit EncoderRatePriorOptions(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        auto* const layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        enabled_ = new QCheckBox(QStringLiteral("预期接收速度先验（仅 PAM4）"), this);
        enabled_->setObjectName(QStringLiteral("expectedReceptionRateEnabled"));
        setToolTip(QStringLiteral("填写以往 Decoder 显示的平均原始接收速度，1 KB = 1024 字节。不是限速或接收确认；仅超过 12 段的 PAM4 生效。"));
        layout->addWidget(enabled_);
        auto* const range = new QHBoxLayout();
        minimum_ = new QSpinBox(this);
        minimum_->setObjectName(QStringLiteral("expectedReceptionRateMinimum"));
        maximum_ = new QSpinBox(this);
        maximum_->setObjectName(QStringLiteral("expectedReceptionRateMaximum"));
        for (auto* const input : {minimum_, maximum_})
        {
            input->setRange(1, 1048576);
            input->setSuffix(QStringLiteral(" KB/s"));
        }
        minimum_->setValue(200);
        maximum_->setValue(280);
        range->addWidget(minimum_);
        range->addWidget(new QLabel(QStringLiteral("～"), this));
        range->addWidget(maximum_);
        range->addStretch();
        layout->addLayout(range);
        hint_ = new QLabel(QStringLiteral("经验值，不是限速或接收确认。超过 12 段时生效；可能更快，也可能更慢。"), this);
        hint_->setObjectName(QStringLiteral("expectedReceptionRateHint"));
        hint_->setWordWrap(true);
        layout->addWidget(hint_);
        connect(enabled_, &QCheckBox::toggled, this, [this]()
        {
            preferenceWarning_.clear();
            UpdateEnabledState();
        });
        UpdateEnabledState();
    }

    void Load(QSettings& settings)
    {
        bool minimumOk = false;
        bool maximumOk = false;
        const auto minimum = settings.value(QStringLiteral("encoder/pam4RatePriorMinimumKibPerSecond"), 200).toString().toUInt(&minimumOk);
        const auto maximum = settings.value(QStringLiteral("encoder/pam4RatePriorMaximumKibPerSecond"), 280).toString().toUInt(&maximumOk);
        const auto enabled = settings.value(QStringLiteral("encoder/pam4RatePriorEnabled"), false).toString().toLower();
        const bool valid = minimumOk && maximumOk && pbapp::ValidateSenderRatePrior({minimum, maximum}) &&
            (enabled == QStringLiteral("true") || enabled == QStringLiteral("false") || enabled == QStringLiteral("1") || enabled == QStringLiteral("0"));
        minimum_->setValue(valid ? static_cast<int>(minimum) : 200);
        maximum_->setValue(valid ? static_cast<int>(maximum) : 280);
        enabled_->setChecked(valid && (enabled == QStringLiteral("true") || enabled == QStringLiteral("1")));
        preferenceWarning_ = valid ? QString() : QStringLiteral("保存的先验无效，已关闭；请核对后重新启用。\n");
        UpdateEnabledState();
    }

    void Save(QSettings& settings) const
    {
        settings.setValue(QStringLiteral("encoder/pam4RatePriorEnabled"), enabled_->isChecked());
        settings.setValue(QStringLiteral("encoder/pam4RatePriorMinimumKibPerSecond"), minimum_->value());
        settings.setValue(QStringLiteral("encoder/pam4RatePriorMaximumKibPerSecond"), maximum_->value());
    }

    void SetContext(const pbapp::VisualProfile profile, const bool editable)
    {
        supported_ = profile == pbapp::VisualProfile::ExperimentalPam4;
        editable_ = editable;
        UpdateEnabledState();
    }

    [[nodiscard]] QString Apply(pbapp::EncoderConfig& config) const
    {
        config.expectedReceptionRate.reset();
        if (config.visualProfile != pbapp::VisualProfile::ExperimentalPam4 || !enabled_->isChecked())
        {
            return {};
        }
        const pbapp::SenderRatePrior prior{static_cast<std::uint32_t>(minimum_->value()), static_cast<std::uint32_t>(maximum_->value())};
        if (!pbapp::ValidateSenderRatePrior(prior))
        {
            return QStringLiteral("预期接收速度下限不能大于上限；单位 KB/s（1024 字节）。");
        }
        config.expectedReceptionRate = prior;
        return {};
    }

private:
    void UpdateEnabledState()
    {
        enabled_->setEnabled(supported_ && editable_);
        minimum_->setEnabled(supported_ && editable_ && enabled_->isChecked());
        maximum_->setEnabled(supported_ && editable_ && enabled_->isChecked());
        hint_->setText(preferenceWarning_ + (supported_ ?
            QStringLiteral("经验值，不是限速或接收确认。超过 12 段时生效；可能更快，也可能更慢。") :
            QStringLiteral("当前模式不应用先验；保留 PAM4 设置。")));
    }

    QCheckBox* enabled_ = nullptr;
    QSpinBox* minimum_ = nullptr;
    QSpinBox* maximum_ = nullptr;
    QLabel* hint_ = nullptr;
    QString preferenceWarning_;
    bool supported_ = true;
    bool editable_ = true;
};

}
