/**
 * @file MainWindow.h
 * @brief 主窗口：图片背景、效果样式、动态壁纸、看板娘和系统托盘的统一设置界面。
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QLabel>
#include <QMainWindow>
#include <QMutex>
#include <QSize>
#include <QSlider>
#include <QStringList>
#include <QVector>

#include <memory>

#include "engine/Engine.h"
#include "core/ImageProcess.h"

namespace kanban {
class KanbanController;
}
class SystemTrayController;

class QLineEdit;
class QListWidget;
class QStackedWidget;
class QCheckBox;
class QComboBox;
class QPushButton;
class QRadioButton;
class QGridLayout;
class QScrollArea;
class QFrame;
class QTimer;
class QButtonGroup;
class QProcess;

/**
 * @brief 主窗口。
 *
 * 承载图片背景、效果样式、动态壁纸、看板娘和系统托盘的全部设置界面。
 * 采用侧边栏导航 + 多页堆栈布局，各页构建逻辑分散在 MainWindowXxxPage.cpp 中。
 *
 * 生命周期：
 * - 析构函数负责关闭图库缩略图后台任务的回调闸门；
 * - closeEvent 根据后台任务状态决定隐藏还是退出；
 * - 退出流程通过 ApplicationShutdown 统一收口。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    // 析构必须存在：它负责关闭图库缩略图后台任务的回调闸门(见 m_thumbTasksLive)。
    ~MainWindow() override;

private slots:
    void switchPage(int row);
    void selectHeaderTab(int index);
    void selectWallTab(int index);
    void addVideos();
    void scanVideoDir(); // 扫描软件目录 data/video 下的视频并入列表
    void removeSelectedVideos();
    void clearVideos();
    void startVideo();
    void stopVideo();
    void refreshVideoList();
    void onVideoStateChanged(const QString &text);
    void updateVideoButtons();
    void updatePlayingHighlight(); // 播放列表中当前曲目条目高亮
    void transcodeSelectedVideo();      // “立即转码”：异步跑 ffmpeg，不阻塞界面
    void onTranscodeOutput();           // 解析进度并回写到按钮文字
    void onTranscodeFinished(int exitCode, bool crashed);
    void updateTranscodeButton();       // 仅当列表里选中一个视频时可点
    void addVideoToPlaylist(const QString &path);
    void pickPresetFolder();
    void rebuildGallery();
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
    void closeEvent(QCloseEvent *event) override; // 保存窗口几何到统一配置
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    // 把当前窗口尺寸/位置写进配置(不含保存后的 cfg.save())
    void saveWindowGeometry();

    // UI builders
    QWidget *buildTitleBar();
    QWidget *buildSidebar();
    QWidget *buildHeader();
    QWidget *buildImagePage();
    QWidget *buildEffectPage();
    QWidget *buildHelpPage();
    QWidget *buildWallpaperPage();
    QWidget *buildVideoWallpaperPage();
    QWidget *buildTranscodeCard(QWidget *parent); // 视频转码模块(左列第二张卡片)
    QWidget *buildWebWallpaperPage();
    QSlider *makeSlider(int min, int max, int value, QLabel **valueLabel,
                        const QString &suffix = QString());

    // helpers
    void updateImagePreview();
    void scheduleImagePreview();          // 合并 resize 期间的预览重绘
    // 预览框宽高比锁死为桌面(主屏)比例，宽度变化时按比例反算高度。
    void updatePreviewAspect();
    void updateEffectPresetSelection(int index);
    void setLog(const QString &text, bool isError);
    void setStatusChips();
    // Hook DLL 目录迁移(<程序目录>\dll)后的启动自检：把当前注册状态记入诊断日志，
    // 若注册表仍指向旧的 %LOCALAPPDATA% 位置，则预投放新 DLL 并提示用户重新应用。
    void reportDllMigration();
    void loadSettings();
    void saveImageSettings();
    void saveEffectSettings();
    void applyTheme(int mode);
    // Shared setup for every QComboBox: names the popup view and its top-level
    // frame, and clips that frame to the rounded panel the stylesheet paints.
    void styleCombo(QComboBox *combo) const;
    Engine::EffectConfig currentEffectConfig() const;
    void setImageSourceText(const QString &text);
    QString elidedTwoLineText(const QString &text, int width) const;
    void setPosMode(int mode);
    void resetImageParams();
    void updateGalleryGrid();
    void uninstallImage();   // 仅卸载图片背景
    void uninstallEffect();  // 仅卸载效果样式
    // 按「调整参数」处理一张原图(亮度/对比度/模糊/转动/尺寸)，单图与随机共用同一条链路。
    QImage applyImageParams(const QImage &src) const;
    // 「随机」模式：把图片浏览列表里的每张图按当前参数处理一遍，铺进图片池目录
    // (Engine::imagePoolDir())。成功返回 true，count 给出池内图片数。
    bool buildRandomImagePool(int *count, QString *error);

    // —— 看板娘 + 系统托盘 ——
    QWidget *buildKanbanPage();
    QWidget *buildKanbanModelCard(QWidget *parent);
    QWidget *buildKanbanParamCard(QWidget *parent);
    // 控制器 + 托盘 + 退出步骤的装配(在 UI 建好之后调用，顺序有讲究)。
    void setupKanbanAndTray();
    void refreshKanbanModels();          // 重扫模型目录并回填下拉框
    void updateKanbanControls();         // 按钮可用性/状态文本(单一出口，别处不直改)
    void setKanbanLog(const QString &text, bool isError);
    void showFromTray();                 // 托盘「显示窗口」/双击图标
    void onTrayQuitRequested();

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

    // 缩略图后台任务存活闸门(任务书 5.4 / 6.3：lambda 捕获裸 this 的悬空风险)：
    // QThreadPool 工作线程在锁内复查标志后才向 this 投递队列回调，析构函数在同一
    // 把锁内翻标志；互斥保证“检查通过则对象必然仍然存活”，否则退出时可能踩到
    // 已销毁的 MainWindow。
    QMutex m_thumbTasksMutex;
    bool m_thumbTasksLive = true;

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
    QFrame *m_previewFrame = nullptr;      // 承载预览图的边框，宽高比跟随桌面
    QSize m_previewRenderSize;             // 预览画布对应的预览区尺寸(防重入)
    QTimer *m_previewDebounce = nullptr;   // resize 风暴里延迟重绘的定时器
    QString m_previewSrcKey;               // 预览源图缓存键：路径+修改时间
    QImage m_previewSrcCache;              // 已解码(必要时缩放)的预览源图
    QSize m_previewSrcNative;              // 缓存对应的原始像素尺寸
    QSlider *m_rotate = nullptr;
    QSlider *m_scale = nullptr;
    QSlider *m_brightness = nullptr;
    QSlider *m_contrast = nullptr;
    QSlider *m_blur = nullptr;
    QSlider *m_opacity = nullptr;
    QPushButton *m_posButtons[7] = {};     // 显示位置按钮(下标=模式 0填充 1居中 2拉伸 3..6四角)
    int m_posMode = 0;                     // 当前显示位置模式
    QCheckBox *m_folderExt = nullptr;
    // 图片背景模式(互斥单选)：单图 / 随机，默认单图。
    QRadioButton *m_imgModeSingle = nullptr;
    QRadioButton *m_imgModeRandom = nullptr;
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
    QColor m_lightColor = QColor(255, 255, 255);
    QColor m_darkColor = QColor(0, 0, 0);
    QPushButton *m_applyEffectBtn = nullptr;

    // video wallpaper page widgets
    QListWidget *m_videoList = nullptr;
    QSlider *m_videoVolume = nullptr;
    // 播放模式(互斥单选，三选一)：单循环 / 列表循环 / 随机，默认单循环
    QRadioButton *m_modeSingle = nullptr;
    QRadioButton *m_modeList = nullptr;
    QRadioButton *m_modeRandom = nullptr;
    QCheckBox *m_fullscreenPauseBox = nullptr;
    QCheckBox *m_batteryBox = nullptr;
    QCheckBox *m_reclaimBox = nullptr;
    QCheckBox *m_affinityBox = nullptr;   // 资源友好模式(限制逻辑核, 重启生效)
    QCheckBox *m_autostartBox = nullptr;
    QComboBox *m_screenModeCombo = nullptr;
    QPushButton *m_playBtn = nullptr;
    QPushButton *m_pauseBtn = nullptr;
    QComboBox *m_fpsBox = nullptr;
    QLabel *m_videoStatus = nullptr;

    // 视频转码模块：帧率(15/24/30/60，默认24) + 音频(无/有，默认无) + 立即转码
    QButtonGroup *m_tcFpsGroup = nullptr;
    QRadioButton *m_tcAudioNo = nullptr;
    QRadioButton *m_tcAudioYes = nullptr;
    QPushButton *m_transcodeBtn = nullptr;
    QProcess *m_transcodeProc = nullptr;
    QString m_transcodeSrc;              // 本次转码的源文件(用于日志与完成后入列)
    QString m_transcodeDst;              // 本次转码的输出文件
    bool m_transcodeHadOutput = false;   // 启动前输出文件是否已存在(失败时不能删旧片)
    qint64 m_transcodeTotalUs = 0;       // 源视频时长(微秒)，用于百分比
    QByteArray m_transcodeBuf;           // ffmpeg 输出按行解析的残留缓冲
    QString m_transcodeErrTail;          // 最后几行 ffmpeg 日志，失败时回显

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
    QSize m_savedWindowSize;  // 启动时恢复的意图尺寸，关闭时保存它以防止布局漂移导致膨胀
    bool m_windowShown = false; // 首次 show 完成前不更新 m_savedWindowSize

    // kanban page widgets
    std::unique_ptr<kanban::KanbanController> m_kanban;
    SystemTrayController *m_tray = nullptr;
    QPushButton *m_kanbanStartBtn = nullptr;
    QPushButton *m_kanbanPauseBtn = nullptr;
    QPushButton *m_kanbanNextBtn = nullptr;
    QPushButton *m_kanbanExprBtn = nullptr;
    QLabel *m_kanbanStatus = nullptr;
    QLabel *m_kanbanLog = nullptr;
    QComboBox *m_kanbanModelCombo = nullptr;
    QLabel *m_kanbanModelInfo = nullptr;
    QSlider *m_kanbanScale = nullptr;
    QSlider *m_kanbanOpacity = nullptr;
    QSlider *m_kanbanFps = nullptr;
    QLabel *m_kanbanScaleVal = nullptr;
    QLabel *m_kanbanOpacityVal = nullptr;
    QLabel *m_kanbanFpsVal = nullptr;
    QCheckBox *m_kanbanTopBox = nullptr;
    QCheckBox *m_kanbanThroughBox = nullptr;
    QCheckBox *m_kanbanInteractBox = nullptr;
    QCheckBox *m_kanbanGazeBox = nullptr;
    QCheckBox *m_kanbanAutoStartBox = nullptr;
    QCheckBox *m_kanbanPauseHiddenBox = nullptr;
    QCheckBox *m_trayAlwaysBox = nullptr;
    QCheckBox *m_trayMinimizeBox = nullptr;
    // 回填设置时挡住「控件变化 = 用户改动」，否则 loadSettings 会把刚读的值再写回去。
    bool m_kanbanSyncing = false;
};

#endif // MAINWINDOW_H
