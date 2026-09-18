/**
 * @file MainWindow.h
 * @brief 主窗口：图片背景、效果样式、动态壁纸、看板娘和系统托盘的统一设置界面。
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QMutex>
#include <QPoint>
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
class QListWidgetItem;
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
    void refreshKanbanModels();          // 重扫模型目录并重建模型网格
    void updateKanbanControls();         // 按钮可用性/状态文本(单一出口，别处不直改)
    void updateKanbanStatus();           // 低频统计只更新状态文本
    void setKanbanLog(const QString &text, bool isError);
    void showFromTray();                 // 托盘图标左键单击/双击
    void onTrayQuitRequested();

    // —— 模型静态预览图 ——
    //
    // 取图逻辑：先按模型文件夹名到 .cache/model-thumbs 找同名 PNG，找不到才生成。
    // 生成由**独立进程**(本程序自己的 --render-model-thumbs 模式)完成，原因见
    // src/kanban/ModelThumbJob.h。这里只负责「发现缺图 → 起进程 → 盯缓存目录
    // → 贴图标」。进度信号是**文件落盘**，不是子进程的标准输出。
    void ensureKanbanModelThumbs(bool force = false);
    void reloadKanbanModelIcons();       // 按当前缓存重贴全部格子图标
    void applyKanbanModelThumb(const QString &modelId); // 单个模型出图后即时贴图
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
    QCheckBox *m_fpsKeepSpeedBox = nullptr; // 限帧方式：保速丢帧 / 慢动作
    QLabel *m_videoStatus = nullptr;

    // 显示器电源通知的订阅句柄(Windows 的 HPOWERNOTIFY)。用 void* 是为了不在头文件
    // 里引 windows.h；空 = 没订上，熄灭/唤醒判据随之不可用(构造时会记一行)。
    void *m_powerNotify = nullptr;

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
    QLabel *m_kanbanStatus = nullptr;
    QLabel *m_kanbanLog = nullptr;
    QListWidget *m_kanbanModelGrid = nullptr;
    QLabel *m_kanbanModelInfo = nullptr;
    // 生成预览图的子进程。**非空即表示正在生成** —— 拿它当唯一的重入闸门，
    // 免得再维护一个布尔量，两个状态迟早打架。
    QProcess *m_kanbanThumbJob = nullptr;
    // 任务期间轮询缓存目录的定时器：某个模型的图一落盘就贴到对应格子上。
    //
    // 为什么用「查文件」而不是「读子进程的 stdout」：本程序是 GUI 子系统的
    // 可执行文件，实测它的 stdout 在 GL 上下文建立之后就写不出去了(见
    // ModelThumbJob.cpp 里 report() 的说明)。而**文件一定写得出来** ——
    // 图片本身就是这么落盘的。于是「缓存目录里出现了这张图」成了唯一可靠的
    // 完成信号，也正好就是用户描述的那条取图逻辑。
    QTimer *m_kanbanThumbPoll = nullptr;
    // 本次任务的基线：模型 id → 任务开始前该缓存文件的修改时间(0 = 当时没有)。
    // 用修改时间而不是「文件在不在」判定完成，是为了让 --force(强制重建，
    // 文件本来就在)也能被识别出来。这个哈希同时充当「还没出图的待办清单」。
    QHash<QString, qint64> m_kanbanThumbBaseline;
    int m_kanbanThumbTotal = 0; // 本次任务一共要出几张(收尾算成绩用)
    QSlider *m_kanbanScale = nullptr;
    QSlider *m_kanbanOpacity = nullptr;
    QSlider *m_kanbanFps = nullptr;
    QLabel *m_kanbanScaleVal = nullptr;
    QLabel *m_kanbanOpacityVal = nullptr;
    QLabel *m_kanbanFpsVal = nullptr;
    QCheckBox *m_kanbanTopBox = nullptr;
    QCheckBox *m_kanbanThroughBox = nullptr;
    QCheckBox *m_kanbanInteractBox = nullptr;
    // 视线追踪是四选一(无/弱/中/强)，所以用互斥单选框而不是复选框：
    // 「多明显」是一条一维刻度，四个选项各自可读，比「一个开关 + 一个灵敏度
    // 滑块」更好选。id 直接用 kanban::KanbanRenderer 的档位值，见构建处。
    QButtonGroup *m_kanbanGazeGroup = nullptr;
    QRadioButton *m_kanbanGazeOff = nullptr;
    QRadioButton *m_kanbanGazeWeak = nullptr;
    QRadioButton *m_kanbanGazeMedium = nullptr;
    QRadioButton *m_kanbanGazeStrong = nullptr;
    QCheckBox *m_trayMinimizeBox = nullptr;
    // 回填设置时挡住「控件变化 = 用户改动」，否则 loadSettings 会把刚读的值再写回去。
    bool m_kanbanSyncing = false;
};

#endif // MAINWINDOW_H
