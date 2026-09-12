#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QLabel>
#include <QMainWindow>
#include <QSize>
#include <QSlider>
#include <QStringList>
#include <QVector>

#include "engine.h"
#include "imageprocess.h"

class QLineEdit;
class QListWidget;
class QStackedWidget;
class QCheckBox;
class QComboBox;
class QPushButton;
class QGridLayout;
class QScrollArea;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void switchPage(int row);
    void selectHeaderTab(int index);
    void selectWallTab(int index);
    void addVideos();
    void removeSelectedVideos();
    void clearVideos();
    void startVideo();
    void stopVideo();
    void refreshVideoList();
    void onVideoStateChanged(const QString &text);
    void updateVideoButtons();
    void pickPresetFolder();
    void rebuildGallery();
    void pickImage();
    void pickWallpaper();
    void selectPreset(int index);
    void applyImage();
    void applyEffect();
    void uninstallAll();
    void refreshStatus();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void changeEvent(QEvent *event) override;

private:
    // UI builders
    QWidget *buildTitleBar();
    QWidget *buildSidebar();
    QWidget *buildHeader();
    QWidget *buildImagePage();
    QWidget *buildEffectPage();
    QWidget *buildHelpPage();
    QWidget *buildWallpaperPage();
    QWidget *buildVideoWallpaperPage();
    QWidget *buildWebWallpaperPage();
    QSlider *makeSlider(int min, int max, int value, QLabel **valueLabel,
                        const QString &suffix = QString());

    // helpers
    void updateImagePreview();
    void updateEffectPresetSelection(int index);
    void setLog(const QString &text, bool isError);
    void setStatusChips();
    void loadSettings();
    void saveImageSettings();
    void saveEffectSettings();
    void applyTheme(int mode);
    Engine::EffectConfig currentEffectConfig() const;
    void setImageSourceText(const QString &text);
    QString elidedTwoLineText(const QString &text, int width) const;
    void setPosMode(int mode);
    void resetImageParams();
    void updateGalleryGrid();

    // data
    struct PresetImage { QString name; QString res; bool isFigure; };
    QVector<PresetImage> m_presets;        // 图库当前内容(浏览目录中的图片)
    QVector<QPushButton *> m_presetButtons;
    int m_selectedPreset = -1; // -1 = custom image
    QString m_customImage;
    QListWidget *m_galleryList = nullptr;
    QString m_presetDir;       // user-chosen extra preset folder
    QString galleryThumbPath(const QString &image) const;
    QString m_sourceText;      // “当前选择”完整文本(展示时按两行省略)

    struct EffectPreset {
        QString name;
        QString desc;
        Engine::EffectConfig cfg;
    };
    QVector<EffectPreset> m_effectPresets;
    QVector<QPushButton *> m_effectButtons;
    int m_selectedEffect = -1; // -1 = custom settings

    // image page widgets
    QLabel *m_previewLabel = nullptr;
    QSize m_previewRenderSize;             // 预览画布对应的预览区尺寸(防重入)
    QSlider *m_rotate = nullptr;
    QSlider *m_scale = nullptr;
    QSlider *m_brightness = nullptr;
    QSlider *m_contrast = nullptr;
    QSlider *m_blur = nullptr;
    QSlider *m_opacity = nullptr;
    QPushButton *m_posButtons[7] = {};     // 显示位置按钮(下标=模式 0填充 1居中 2拉伸 3..6四角)
    int m_posMode = 0;                     // 当前显示位置模式
    QCheckBox *m_folderExt = nullptr;
    QCheckBox *m_comboEffect = nullptr;   // layer blur/mica under the image
    QLabel *m_imageSourceLabel = nullptr;
    QLabel *m_rotateVal = nullptr;
    QLabel *m_scaleVal = nullptr;
    QLabel *m_brightnessVal = nullptr;
    QLabel *m_contrastVal = nullptr;
    QLabel *m_blurVal = nullptr;
    QLabel *m_opacityVal = nullptr;
    QPushButton *m_applyImageBtn = nullptr;

    // effect page widgets
    QComboBox *m_effectCombo = nullptr;    QPushButton *m_lightColorBtn = nullptr;
    QPushButton *m_darkColorBtn = nullptr;
    QSlider *m_lightAlpha = nullptr;
    QSlider *m_darkAlpha = nullptr;
    QLabel *m_lightAlphaVal = nullptr;
    QLabel *m_darkAlphaVal = nullptr;
    QCheckBox *m_clearAddress = nullptr;
    QCheckBox *m_clearBarBg = nullptr;
    QCheckBox *m_clearWinUIBg = nullptr;
    QCheckBox *m_showLine = nullptr;
    QCheckBox *m_keepImage = nullptr;     // also apply the image background
    QColor m_lightColor = QColor(255, 255, 255);
    QColor m_darkColor = QColor(0, 0, 0);
    QPushButton *m_applyEffectBtn = nullptr;

    // video wallpaper page widgets
    QListWidget *m_videoList = nullptr;
    QSlider *m_videoVolume = nullptr;
    QCheckBox *m_autoLoopBox = nullptr;
    QCheckBox *m_randomBox = nullptr;
    QCheckBox *m_fullscreenPauseBox = nullptr;
    QCheckBox *m_batteryBox = nullptr;
    QCheckBox *m_reclaimBox = nullptr;
    QCheckBox *m_autostartBox = nullptr;
    QComboBox *m_screenModeCombo = nullptr;
    QPushButton *m_playBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QComboBox *m_fpsBox = nullptr;
    QLabel *m_videoStatus = nullptr;

    // shell
    QWidget *m_titleBar = nullptr;          // 自绘标题栏(无边框窗口)
    QPushButton *m_titleMin = nullptr;
    QPushButton *m_titleMax = nullptr;
    QPushButton *m_titleClose = nullptr;
    bool m_edgeCursorActive = false;        // 边缘悬停时光标形状已切换
    bool m_resizing = false;                // 手动边缘缩放进行中
    bool m_moving = false;                  // 手动标题栏拖动进行中
    Qt::Edges m_resizeEdges;                // 缩放方向
    QRect m_resizeStartGeom;                // 缩放起始几何
    QPoint m_resizeStartPos;                // 缩放起始鼠标位置
    QPoint m_moveOffset;                    // 拖动时鼠标相对窗口左上角的偏移
    QListWidget *m_nav = nullptr;
    QStackedWidget *m_topStack = nullptr;   // 文件夹美化 / 动态壁纸
    QStackedWidget *m_stack = nullptr;      // 图片背景 / 效果样式 / 使用说明
    QVector<QPushButton *> m_headerTabs;
    QStackedWidget *m_wallStack = nullptr;  // 视频壁纸 / 动态网页壁纸
    QVector<QPushButton *> m_wallTabs;
    QWidget *m_statusBox = nullptr;         // 状态徽标(仅文件夹美化页显示)
    QLabel *m_imageChip = nullptr;
    QLabel *m_effectChip = nullptr;
    QLabel *m_logLabel = nullptr;
    QLabel *m_adminLabel = nullptr;
    QLabel *m_osLabel = nullptr;
    QComboBox *m_themeCombo = nullptr;
    int m_themeMode = 0;      // 0 = follow system, 1 = light, 2 = dark
    bool m_darkTheme = true;  // currently applied scheme
};

#endif // MAINWINDOW_H
