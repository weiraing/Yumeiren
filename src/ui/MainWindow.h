/**
 * @file MainWindow.h
 * @brief 主窗口：图片背景、效果样式、动态壁纸、看板娘和系统托盘的统一设置界面。
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QColor>
#include <QHash>
#include <QLabel>
#include <QPair>
#include <QMainWindow>
#include <QMutex>
#include <QPoint>
#include <QSize>
#include <QSlider>
#include <QStringList>
#include <functional>
#include <QVector>

#include <memory>

#include "explorerbg/Engine.h"
#include "explorerbg/ImageProcess.h"

namespace kanban {
class KanbanController;
}
class SystemTrayController;

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QHBoxLayout;
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
class QKeySequenceEdit;
class QTreeWidget;
class MediaLibraryCard;

namespace fbswin {
class GlobalHotkey;
}

/**
 * @brief 主窗口。
 *
 * 承载图片背景、效果样式、动态壁纸、看板娘和系统托盘的全部设置界面，
 * 采用侧边栏导航 + 多页堆栈布局，各页构建逻辑分散在 MainWindowXxxPage.cpp 中。
 * 析构函数负责关闭图库缩略图后台任务的回调闸门；退出流程通过 ApplicationShutdown 统一收口。
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
    void selectKanbanTab(int index); // 看板娘页内：看板娘 / 设置(实验性功能)
    void scanVideoDir(); // 以 data/video 为准重建视频列表(目录镜像)
    void removeCheckedVideos();
    void startVideo();
    void stopVideo();
    void refreshVideoList();
    void onVideoStateChanged(const QString &text);
    void updateVideoLibraryInfo(); // 视频库页脚：统计信息 · 状态 · 当前视频名
    void updateVideoButtons();
    void updatePlayingHighlight(); // 播放列表中当前曲目条目高亮
    void pickPresetFolder();
    void rebuildGallery();
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
    QWidget *buildWebWallpaperPage();
    void updateWebWallpaperControls(); // 网页壁纸按钮/状态与运行态同步
    void updateWebLibraryInfo();       // 网页库页脚：统计信息 · 状态 · 当前来源名
    // 帧率档位吸附：把配置里存着的、已不在可选档位里的值
    // (如老版本的 15 FPS)吸到最近的档位。视频页与网页页共用。
    static int snapToFpsOption(int saved);
    void refreshWebLibrary();          // 扫描 data/web 下的页面与 Web 项目
    void setWebSource(const QString &source, bool activate);
    void removeCheckedWebItems();      // 删除网页库勾选项(移入回收站)
    QSlider *makeSlider(int min, int max, int value, QLabel **valueLabel,
                        const QString &suffix = QString());


    void updateImagePreview();
    void scheduleImagePreview();

    void updatePreviewAspect();
    void applyPreviewAspect();
    void updateEffectPresetSelection(int index);
    void setLog(const QString &text, bool isError);
    // 写一条「就绪」提示，并记住「这行现在是提示」—— 切页签时只有这种状态下才会被换成
    // 该页的提示；任何 setLog() 写的结果消息都原样留着，见 selectHeaderTab。
    void setLogHint(const QString &text);
    // 两个页签各自的「就绪」提示。单一出处：初始文案与切页签时的替换都用它。
    static QString imagePageHintText();
    static QString effectPageHintText();
    void setStatusChips();
    void setWallStatusChips();   // 动态壁纸页右上角的运行状态徽标(视频/网页各一枚)
    // 三组页内页签(图片背景/动态壁纸/看板娘)共用的构建与选中骨架，见 MainWindow.cpp
    QVector<QPushButton *> makeTabGroup(QHBoxLayout *lay, const QStringList &labels,
                                        bool hiddenByDefault,
                                        const std::function<void(int)> &onSelect);
    bool selectTabGroup(QVector<QPushButton *> &tabs, QStackedWidget *stack, int index);

    void reportDllMigration();
    void loadSettings();
    void saveImageSettings();
    void saveEffectSettings();
    void applyTheme(int mode);

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

    QWidget *buildKanbanPage();
    QWidget *buildKanbanMainPage();  // 页签 0：看板娘主体
    QWidget *buildKanbanLabPage();   // 页签 1：设置(实验性功能占位)
    QWidget *buildKanbanModelCard(QWidget *parent);
    QWidget *buildKanbanParamCard(QWidget *parent);
    // 控制器 + 托盘 + 退出步骤的装配(在 UI 建好之后调用，顺序有讲究)。
    void setupKanbanAndTray();
    void refreshKanbanModels();          // 重扫模型目录并重建模型网格
    void updateKanbanControls();         // 按钮可用性/状态文本(单一出口，别处不直改)
    // 只重写底部状态栏(第一行现算 + 第二行取缓存)，挂在帧率信号上每秒一次。
    void updateKanbanStatus();
    void setKanbanLog(const QString &text, bool isError);
    void showFromTray();                 // 托盘图标左键单击/双击
    void onTrayQuitRequested();

    // 取图逻辑：先按 thumbKey(模型目录相对 data/models 的路径，分隔符换 '#')到
    // .cache/model-thumbs 找同名 PNG，找不到才生成。生成由独立进程(本程序自己的
    // --render-model-thumbs 模式)完成，原因见 src/kanban/ModelThumbJob.h。
    // 进度信号是文件落盘，不是子进程的标准输出。
    void ensureKanbanModelThumbs(bool force = false);
    void reloadKanbanModelIcons();       // 按当前缓存重贴全部格子图标
    void applyKanbanModelThumb(const QString &thumbKey); // 单个模型出图后即时贴图
    void pollKanbanModelThumbs();        // 任务期间轮询：已落盘的先贴上去
    void onKanbanThumbFinished(int exitCode);
    void showKanbanModelMenu(const QPoint &viewportPos); // 模型卡片右键菜单
    void deleteKanbanModel(QListWidgetItem *item);       // 删除模型文件夹 + 缓存预览图

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
    // 预览框高度调整必须推迟到事件循环(见 updatePreviewAspect)：在 Resize 事件里同步
    // setFixedHeight 会形成同步自激环，拖窗口时直接 0xC0000005 崩掉。
    QTimer *m_previewAspectTimer = nullptr;
    int m_previewAspectPending = 0;        // 待应用的高度(px)，<=0 表示无待办
    QString m_previewSrcKey;               // 预览源图缓存键：路径+修改时间
    QImage m_previewSrcCache;              // 已解码(必要时缩放)的预览源图
    QSize m_previewSrcNative;              // 缓存对应的原始像素尺寸
    QSlider *m_rotate = nullptr;
    QSlider *m_scale = nullptr;
    QSlider *m_brightness = nullptr;
    QSlider *m_contrast = nullptr;
    QSlider *m_blur = nullptr;
    QSlider *m_transparency = nullptr;     // 图片背景透明度(%)，0=不透明；要 alpha 时经换算
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
    QLabel *m_transparencyVal = nullptr;
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
    QTreeWidget *m_videoList = nullptr;      // 指向 MediaLibraryCard 内的列表
    MediaLibraryCard *m_videoLib = nullptr;
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
    // 动态网页壁纸页(WebView2)
    QButtonGroup *m_webRefreshGroup = nullptr;  // 刷新策略：互斥单选，id 就是 WebWallpaper::RefreshMode 枚举值
    QButtonGroup *m_webInteractGroup = nullptr;  // 交互模式：互斥单选，id 0=网页展示(穿透) / 1=网页交互
    QButtonGroup *m_webFpsGroup = nullptr;  // 帧率：互斥单选，按钮 id 就是 fps 值(0=跟随页面)
    QSlider *m_webVolumeSlider = nullptr;
    QLabel *m_webVolumeVal = nullptr;
    QSlider *m_webZoomSlider = nullptr;
    QLabel *m_webZoomVal = nullptr;
    QTreeWidget *m_webTree = nullptr;        // 指向 MediaLibraryCard 内的列表
    MediaLibraryCard *m_webLib = nullptr;
    // 网页库页脚第一段(统计信息)。页面/项目数在 refreshWebLibrary 里数出来存这儿，
    // 状态与来源名由 updateWebLibraryInfo() 现取 —— 三段凑齐后交给 card->setInfo()。
    QString m_webStatsText;
    QPushButton *m_webStartBtn = nullptr;
    QLabel *m_webStateLabel = nullptr;
    bool m_webRuntimeOk = false;
    QPushButton *m_playBtn = nullptr;
    QPushButton *m_pauseBtn = nullptr;
    QButtonGroup *m_fpsGroup = nullptr;     // 帧率：互斥单选，按钮 id 就是 fps 值(0=跟随视频帧率)
    QCheckBox *m_fpsKeepSpeedBox = nullptr; // 限速方式：保速丢帧 / 慢动作
    // 视频库页脚第二段(状态)：壁纸层最后一次 playbackStateChanged 的原文。
    // 存原文而不是自己拼短词 —— 「检测到全屏应用，已自动暂停」这类"为什么"不能丢。
    QString m_videoStateText;

    // 显示器电源通知的订阅句柄(Windows 的 HPOWERNOTIFY)。用 void* 是为了不在头文件
    // 里引 windows.h；空 = 没订上，熄灭/唤醒判据随之不可用(构造时会记一行)。
    void *m_powerNotify = nullptr;

    // shell
    QWidget *m_titleBar = nullptr;          // 自绘标题栏(无边框窗口)
    QPushButton *m_titleMin = nullptr;
    QPushButton *m_titleMax = nullptr;
    QPushButton *m_titleClose = nullptr;
    QListWidget *m_nav = nullptr;
    QStackedWidget *m_topStack = nullptr;   // 文件夹美化 / 动态壁纸
    QStackedWidget *m_stack = nullptr;      // 图片背景 / 效果样式 / 使用说明
    QVector<QPushButton *> m_headerTabs;
    QStackedWidget *m_wallStack = nullptr;  // 视频壁纸 / 动态网页壁纸
    QVector<QPushButton *> m_wallTabs;
    QStackedWidget *m_kanbanStack = nullptr; // 看板娘 / 设置(实验性功能)
    QVector<QPushButton *> m_kanbanTabs;
    QWidget *m_statusBox = nullptr;         // 状态徽标(仅文件夹美化页显示)
    QLabel *m_imageChip = nullptr;
    QLabel *m_effectChip = nullptr;
    QWidget *m_wallStatusBox = nullptr;     // 运行状态徽标(仅动态壁纸页显示)
    QLabel *m_videoChip = nullptr;
    QLabel *m_webChip = nullptr;
    QLabel *m_logLabel = nullptr;
    // 底部那行现在是不是一条「就绪」提示(而不是某次操作的结果消息)。
    bool m_logShowsHint = false;
    QLabel *m_adminLabel = nullptr;
    QLabel *m_osLabel = nullptr;
    QLabel *m_versionLabel = nullptr;
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
    QLabel *m_kanbanLog = nullptr;
    QListWidget *m_kanbanModelGrid = nullptr;

    QLabel *m_kanbanModelInfo = nullptr;
    QString m_kanbanModelLine;
    // 生成预览图的子进程。**非空即表示正在生成** —— 拿它当唯一的重入闸门。
    QProcess *m_kanbanThumbJob = nullptr;

    QTimer *m_kanbanThumbPoll = nullptr;

    QHash<QString, qint64> m_kanbanThumbBaseline;
    int m_kanbanThumbTotal = 0; // 本次任务一共要出几张(收尾算成绩用)
    QSlider *m_kanbanScale = nullptr;
    QSlider *m_kanbanTransparency = nullptr;
    QSlider *m_kanbanFps = nullptr;
    QSlider *m_kanbanMenuOpacity = nullptr;
    QLabel *m_kanbanMenuOpacityVal = nullptr;
    QSlider *m_kanbanGlass = nullptr;
    QLabel *m_kanbanGlassVal = nullptr;
    QList<QPair<QPushButton *, QColor>> m_kanbanBgSwatches;
    QLabel *m_kanbanScaleVal = nullptr;
    QLabel *m_kanbanTransparencyVal = nullptr;
    QLabel *m_kanbanFpsVal = nullptr;
    QCheckBox *m_kanbanTopBox = nullptr;
    QCheckBox *m_kanbanThroughBox = nullptr;
    QCheckBox *m_kanbanInteractBox = nullptr;
    QCheckBox *m_kanbanMotionLoopBox = nullptr;
    QCheckBox *m_kanbanSoundBox = nullptr;
    QCheckBox *m_kanbanDoubleClickBox = nullptr;
    // 「按清单隐藏网格」总开关；清单本身在模型目录的 *.hidden.json 里，不是界面上的数据。
    QCheckBox *m_kanbanMeshHideBox = nullptr;
    // 视线追踪是四选一(无/弱/中/强)，用互斥单选框而不是复选框：这是一条一维刻度。
    // id 直接用 kanban::KanbanRenderer 的档位值，见构建处。
    QButtonGroup *m_kanbanGazeGroup = nullptr;
    QRadioButton *m_kanbanGazeOff = nullptr;
    QRadioButton *m_kanbanGazeWeak = nullptr;
    QRadioButton *m_kanbanGazeMedium = nullptr;
    QRadioButton *m_kanbanGazeStrong = nullptr;
    QCheckBox *m_trayMinimizeBox = nullptr;
    QCheckBox *m_kanbanAutostartBox = nullptr; // 与壁纸页 m_autostartBox 同一个注册表项
    QKeySequenceEdit *m_kanbanHotkeyEdit = nullptr;
    fbswin::GlobalHotkey *m_kanbanHotkey = nullptr; // 显示/隐藏看板娘的全局快捷键
    // 回填设置时挡住「控件变化 = 用户改动」，否则会把刚读的值再写回去。
    bool m_kanbanSyncing = false;
};

#endif // MAINWINDOW_H
