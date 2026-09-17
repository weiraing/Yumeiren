// MainWindow 效果样式页的构建与交互逻辑。
#include "MainWindow.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "engine/Engine.h"
#include "ui/TooltipStyle.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>


QWidget *MainWindow::buildEffectPage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    // ---- preset cards ----
    auto *card = new QFrame(page);
    card->setObjectName(QStringLiteral("PageCard"));
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(14, 12, 14, 14);
    cardLay->setSpacing(8);

    auto *t1 = new QLabel(QStringLiteral("预设效果样式(来自 ExplorerBlurMica)"), card);
    t1->setObjectName(QStringLiteral("GroupTitle"));
    cardLay->addWidget(t1);
    auto *hint = new QLabel(
        QStringLiteral("为资源管理器窗口添加 Blur / Acrylic / Mica 背景效果，兼容 Win10 与 Win11。"
                       "Mica 仅 Win11 可用；Win11 23H2 以上建议开启“清除 WinUI 工具栏背景”。"),
        card);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    cardLay->addWidget(hint);

    auto *flow = new QGridLayout();
    flow->setSpacing(10);
    for (int i = 0; i < m_effectPresets.size(); ++i) {
        const auto &p = m_effectPresets[i];
        auto *btn = new QPushButton(card);
        btn->setObjectName(QStringLiteral("PresetCard"));
        btn->setCheckable(true);
        btn->setMinimumSize(260, 62);
        btn->setText(QStringLiteral("%1\n%2").arg(p.name, p.desc));
        btn->setToolTip(tooltipstyle::format(p.desc));
        connect(btn, &QPushButton::clicked, this, [this, i] {
            m_selectedEffect = i;
            updateEffectPresetSelection(i);
            const auto &cfg = m_effectPresets[i].cfg;
            m_effectCombo->setCurrentIndex(cfg.effect);
            m_clearAddress->setChecked(cfg.clearAddress);
            m_clearBarBg->setChecked(cfg.clearBarBg);
            m_clearWinUIBg->setChecked(cfg.clearWinUIBg);
            m_showLine->setChecked(cfg.showLine);
            m_lightColor = QColor(cfg.lightR, cfg.lightG, cfg.lightB);
            m_darkColor = QColor(cfg.darkR, cfg.darkG, cfg.darkB);
            m_lightAlpha->setValue(cfg.lightA);
            m_darkAlpha->setValue(cfg.darkA);
            m_lightColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(m_lightColor.name()));
            m_darkColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(m_darkColor.name()));
            setLog(QStringLiteral("已选择预设：%1，点击“应用效果”生效。")
                       .arg(m_effectPresets[i].name), false);
        });
        m_effectButtons.append(btn);
        flow->addWidget(btn, i / 2, i % 2);
    }
    flow->setRowStretch(flow->rowCount(), 1);
    cardLay->addLayout(flow);

    // ---- custom settings ----
    auto *t2 = new QLabel(QStringLiteral("自定义参数"), card);
    t2->setObjectName(QStringLiteral("GroupTitle"));
    cardLay->addWidget(t2);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    grid->addWidget(new QLabel(QStringLiteral("效果类型"), card), 0, 0);
    m_effectCombo = new QComboBox(card);
    m_effectCombo->addItems({QStringLiteral("0 - Blur 模糊(≤Win11 22H2)"),
                             QStringLiteral("1 - Acrylic 亚克力"),
                             QStringLiteral("2 - Mica 云母(仅 Win11)"),
                             QStringLiteral("3 - Blur(Clear) 纯模糊"),
                             QStringLiteral("4 - MicaAlt(仅 Win11)")});
    m_effectCombo->setCurrentIndex(1);
    styleCombo(m_effectCombo);
    grid->addWidget(m_effectCombo, 0, 1, 1, 3);

    m_lightColorBtn = new QPushButton(QStringLiteral("亮色混合"), card);
    m_darkColorBtn = new QPushButton(QStringLiteral("暗色混合"), card);
    m_lightColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_lightColor.name()));
    m_darkColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_darkColor.name()));
    m_lightAlpha = makeSlider(0, 255, 200, &m_lightAlphaVal);
    m_darkAlpha = makeSlider(0, 255, 120, &m_darkAlphaVal);
    grid->addWidget(m_lightColorBtn, 1, 0);
    grid->addWidget(new QLabel(QStringLiteral("亮色透明度"), card), 1, 1);
    grid->addWidget(m_lightAlpha, 1, 2);
    grid->addWidget(m_lightAlphaVal, 1, 3);
    grid->addWidget(m_darkColorBtn, 2, 0);
    grid->addWidget(new QLabel(QStringLiteral("暗色透明度"), card), 2, 1);
    grid->addWidget(m_darkAlpha, 2, 2);
    grid->addWidget(m_darkAlphaVal, 2, 3);

    auto *optRow = new QHBoxLayout();
    m_clearAddress = new QCheckBox(QStringLiteral("清除地址栏背景"), card);
    m_clearBarBg = new QCheckBox(QStringLiteral("清除滚动条背景"), card);
    m_clearWinUIBg = new QCheckBox(QStringLiteral("清除 WinUI 工具栏背景(Win11)"), card);
    m_showLine = new QCheckBox(QStringLiteral("显示树视图分割线"), card);
    m_clearAddress->setChecked(true);
    m_clearBarBg->setChecked(true);
    m_clearWinUIBg->setChecked(true);
    optRow->addWidget(m_clearAddress);
    optRow->addWidget(m_clearBarBg);
    optRow->addWidget(m_clearWinUIBg);
    optRow->addWidget(m_showLine);
    grid->addLayout(optRow, 3, 0, 1, 4);
    cardLay->addLayout(grid);

    connect(m_lightColorBtn, &QPushButton::clicked, this, [this] {
        QColor c = QColorDialog::getColor(m_lightColor, this, QStringLiteral("亮色模式混合色"));
        if (c.isValid()) {
            m_lightColor = c;
            m_lightColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(c.name()));
            m_selectedEffect = -1;
            updateEffectPresetSelection(-1);
        }
    });
    connect(m_darkColorBtn, &QPushButton::clicked, this, [this] {
        QColor c = QColorDialog::getColor(m_darkColor, this, QStringLiteral("暗色模式混合色"));
        if (c.isValid()) {
            m_darkColor = c;
            m_darkColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(c.name()));
            m_selectedEffect = -1;
            updateEffectPresetSelection(-1);
        }
    });

    lay->addWidget(card);

    // ---- apply ----
    auto *btnCard = new QFrame(page);
    btnCard->setObjectName(QStringLiteral("PageCard"));
    auto *btnLay = new QHBoxLayout(btnCard);
    btnLay->setContentsMargins(14, 12, 14, 12);
    btnLay->setSpacing(10);
    m_applyEffectBtn = new QPushButton(QStringLiteral("✓ 应用效果样式"), btnCard);
    m_applyEffectBtn->setObjectName(QStringLiteral("PrimaryButton"));
    auto *resetBtn = new QPushButton(QStringLiteral("↩ 恢复"), btnCard);
    resetBtn->setObjectName(QStringLiteral("DangerButton"));
    connect(m_applyEffectBtn, &QPushButton::clicked, this, &MainWindow::applyEffect);
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::uninstallEffect);
    btnLay->addWidget(m_applyEffectBtn, 1);
    btnLay->addWidget(resetBtn, 1);
    lay->addWidget(btnCard);

    lay->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}
void MainWindow::updateEffectPresetSelection(int index)
{
    for (int i = 0; i < m_effectButtons.size(); ++i)
        m_effectButtons[i]->setChecked(i == index);
}
void MainWindow::applyEffect()
{
    Engine::EffectConfig cfg = currentEffectConfig();

    m_applyEffectBtn->setEnabled(false);
    setLog(QStringLiteral("正在写入配置…"), false);
    QCoreApplication::processEvents();

    QString err;
    if (!Engine::instance().writeEffectConfig(cfg, &err)) {
        setLog(err, true);
        m_applyEffectBtn->setEnabled(true);
        return;
    }
    setLog(QStringLiteral("正在注册 DLL(需要管理员权限)…"), false);
    QCoreApplication::processEvents();

    // 只动特效 Hook：图片 Hook 的注册状态与配置原样保留，两页互不影响。
    saveEffectSettings();
    if (!Engine::instance().registerEffectDll(&err)) {
        setLog(err, true);
        m_applyEffectBtn->setEnabled(true);
        return;
    }

    setLog(QStringLiteral("正在重启资源管理器…"), false);
    QCoreApplication::processEvents();
    Engine::restartExplorer(nullptr);
    refreshStatus();
    setLog(QStringLiteral("效果样式已应用！打开任意文件夹即可查看效果。"), false);
    m_applyEffectBtn->setEnabled(true);
}
