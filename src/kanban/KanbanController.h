// 看板娘控制器：拥有窗口与渲染器，窗口只借用渲染器。
// 运行状态由状态机管理，动画仅由统一时钟推进；Live2D 失败时降级为占位后端。
#ifndef KANBANCONTROLLER_H
#define KANBANCONTROLLER_H

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>

#include <QElapsedTimer>

#include <memory>

#include "kanban/KanbanModelManager.h"
#include "kanban/KanbanStateMachine.h"
#include "kanban/KanbanTypes.h"

class QTimer;

namespace kanban {

class KanbanAnimationClock;
class KanbanRenderer;
class KanbanWindow;

class KanbanController : public QObject
{
    Q_OBJECT

public:
    explicit KanbanController(QObject *parent = nullptr);
    ~KanbanController() override;

    // —— 查询(界面/托盘只读这里，不碰内部对象) ——
    bool isRunning() const { return m_machine.isRunning(); }
    bool isPaused() const { return m_machine.isPaused(); }
    bool isVisible() const;
    State state() const { return m_machine.state(); }
    QString stateText() const { return kanban::stateText(m_machine.state()); }
    QString backendText() const { return m_backendName; }
    bool live2dAvailable() const;
    QString currentModelName() const { return m_currentModelName; }
    QStringList modelNames() const;
    int expressionCount() const;
    // 有效模型的完整信息，顺序与 modelNames() 一致。
    QVector<ModelInfo> validModelList() const;
    int measuredFps() const;
    QString lastError() const { return m_lastError; }

    // —— 生命周期 ——
    bool start();       // 启动(含降级)；返回 false 表示两者都起不来
    bool retry();       // Error 态下重试
    void pauseResume(); // 暂停/恢复(不销毁窗口)
    void stop();        // 取消看板娘：停帧、释放模型、关窗、复位
    void playNext();    // 切换动作；没有可播动作就到此为止(不换模型)
    void playNextExpression(); // 下一个表情；没有表情就静默返回(不换模型)
    // 实际装载成功的动作数(含 idle)与「这个入口该不该可点」。界面拿它统计和置灰。
    int playableMotionCount() const;
    bool canPlayNextMotion() const;
    int currentMotionOrdinal() const;
    void setMotionLoopEnabled(bool enabled);
    bool motionLoopEnabled() const { return m_motionLoopEnabled; }
    void setSoundEnabled(bool enabled);
    bool soundEnabled() const { return m_soundEnabled; }
    void setDoubleClickSwitchEnabled(bool enabled);
    bool doubleClickSwitchEnabled() const { return m_doubleClickSwitchEnabled; }
    // 只有 start() 会调用(软件渲染后端启动的最后一步)。
    void showWindow();
    // 全局快捷键的显示/隐藏切换：没在跑就直接 start()(快捷键即「召唤」)；在跑则
    // hide/show 窗口。隐藏时模型与位置都保留，帧时钟停掉(GPU 归零)，show 后按状态
    // 重启 —— 与托盘的暂停不同，这里不进暂停态，托盘菜单语义不受影响。
    void toggleVisible();

    // —— 设置(全部即时生效并落盘) ——
    void setScalePercent(int percent);
    int scalePercent() const { return m_scalePercent; }
    void setTransparencyPercent(int percent);
    void setMenuBgColor(const QColor &color);
    // 菜单透明度：0=不透明，80=最透。与窗口透明度的方向、上限保持一致。
    void setMenuTransparency(int percent);
    // 玻璃效果：0=关，越大磨砂感越强。
    void setMenuGlass(int level);
    int transparencyPercent() const { return m_transparencyPercent; }
    QColor menuBgColor() const { return m_menuBg; }
    int menuTransparency() const { return m_menuTransparency; }
    int menuGlass() const { return m_menuGlass; }
    void setTargetFps(int fps);
    int targetFps() const { return m_targetFps; }
    void setAlwaysOnTop(bool onTop);
    bool alwaysOnTop() const { return m_alwaysOnTop; }
    void setMouseThrough(bool through);
    bool mouseThrough() const { return m_mouseThrough; }
    void setInteractionEnabled(bool enabled);
    bool interactionEnabled() const { return m_interactionEnabled; }
    // 读取上次运行状态：停止或错误时清除，正常退出时保留，首次安装为 false。
    bool wasRunningLastTime() const;
    // 视线强度：0=无、1=弱、2=中、3=强；开启时每帧采样全局光标。
    void setGazeStrength(int strength);
    int gazeStrength() const { return m_gazeStrength; }
    // 「有没有开」= 档位 > 0。
    bool gazeTracking() const { return m_gazeStrength != 0; }
    // 按清单隐藏网格的总开关。清单本身是**每个模型一份**的：模型目录里的 *.hidden.json
    // (由 tools/live2d-part-inspector 导出)，装模型时读取，改文件要重新装载才生效。
    // 刻意不做成员镜像：与 textureDownscale 一样，以进程级标志为唯一来源，
    // 免得「控制器以为开着、渲染器以为关着」这类分叉。
    void setMeshHideEnabled(bool enabled);
    // 定义放在 .cpp：读的是 KanbanRenderer.h 里的进程级标志，而这个头刻意不引入渲染器头
    // （只有 .cpp 才需要它）。
    bool meshHideEnabled() const;
    // 当前模型命中的清单摘要(界面状态行用)；本模型没有清单文件时为空串。
    QString meshHideText() const;
    bool setModelPath(const QString &modelJsonPath);
    QString modelPath() const { return m_modelPath; }
    int refreshModels(); // 重新扫描模型目录，返回可用模型数

    // 显示器电源状态(由主窗口的 WM_POWERBROADCAST 投递，与视频壁纸同一个事件源)。
    void setMonitorOn(bool on);

    void loadSettings(); // 从配置读回全部看板娘设置
    void shutdownForExit(); // 先释放 GL 资源再销毁窗口，保留自动恢复设置

signals:
    void measuredFpsChanged(); // 时钟统计更新，仅刷新状态文本
    void runningChanged(bool running);
    void pausedChanged(bool paused);
    void stateChanged(const QString &stateText);
    void backendChanged(const QString &backendText);
    void currentModelChanged(const QString &modelName);
    void settingsChanged();      // 需要回写界面上的滑块/复选框
    void openSettingsRequested(); // 右键菜单「打开设置」
    void quitKanbanRequested();

private slots:
    void onFrameTick(float deltaSeconds);
    void onGlContextReady();
    // 挂起判定心跳(1s)。判据与节奏跟视频壁纸同一套，但各自独立跑：看板娘可以
    // 在壁纸停着的情况下单独运行，反过来也一样。
    void evaluateSuspend();

private:
    // 挂起原因位。刻意只有这两项：锁屏/熄屏是**必然看不见**且判据本机可实测。
    // 「前台全屏 / 桌面被遮挡」不做(看板娘默认置顶，遮挡判定又依赖多屏语义，
    // 本机单屏验证不了)。也刻意不看主窗口有没有收进托盘(看板娘是独立桌面窗口)。
    enum SuspendReason {
        SuspendLocked = 1,
        SuspendMonitorOff = 2,
    };
    // 释放模型与纹理(显存与常驻内存的大头)，保留渲染器与窗口，回来时重新装载。
    void releaseForSuspend();
    void restoreFromSuspend();
    qint64 suspendReleaseThresholdMs();
    // 仅由环境变量驱动的假「看不见」信号，为的是让释放/恢复这条链能在无人
    // 值守的实测里跑到；变量没设时恒为 0。
    int fakeSuspendReasons();

    // 组装/拆解
    bool ensureWindow();
    void destroyWindow();
    bool pickRenderer();                 // Live2D 可用则用，否则占位
    bool initializeAndLoad();            // 只做需要当前 GL 上下文的部分
    bool activateKanban();               // 收尾：定尺寸/起时钟/广播，不能跑在 GL 回调里
    bool fallbackToPlaceholder();        // Live2D 失败时降级
    void enterError(const QString &reason);
    void publishState();

    // 交互事件处置
    void handleClicked(const QPointF &localPos);
    void handleScaleStepped(int steps);
    void saveGeometry();
    void placeWindowFromConfig(); // 建窗后、挂视图前：给窗口一个非零尺寸与落点
    void applyScaleToWindow();
    // 视线追踪：把全局光标换算成窗口坐标交给渲染器。每帧调一次。
    void feedGazeTarget();
    KanbanStateMachine m_machine;
    KanbanModelManager m_models;
    std::unique_ptr<KanbanRenderer> m_renderer;
    KanbanAnimationClock *m_clock = nullptr;
    KanbanWindow *m_window = nullptr;

    QString m_backendName;
    QString m_currentModelName;
    QString m_modelPath;
    QString m_lastError;
    bool m_waitingGl = false;
    bool m_runningPublished = false;
    bool m_pausedPublished = false;

    // —— 挂起(锁屏/熄屏) ——
    QTimer *m_suspendTimer = nullptr;    // 1s 心跳，只在跑起来时挂着
    // 本次持续挂起的时长。无父对象可挂，所以用 unique_ptr 管起来 —— 之前是裸 new，
    // 一直没人 delete(进程级对象，泄漏量小但确实是漏的)。
    std::unique_ptr<QElapsedTimer> m_suspendClock;
    int m_suspendReasons = 0;
    bool m_releasedForSuspend = false;
    bool m_monitorOn = true;
    // 假挂起窗口的起算表(见 fakeSuspendReasons)，只在设了环境变量时走动。
    QElapsedTimer m_fakeSuspendClock;

    // 设置镜像(落盘的单一来源；改设置走 setter，别直接写这几个字段)
    int m_scalePercent = 100;
    int m_transparencyPercent = 0;
    QColor m_menuBg;       // 右键菜单底色(无效 = 主题默认)
    int m_menuTransparency = 0;  // 菜单透明度：0=不透明，80=最透
    int m_menuGlass = 0;   // 0=关
    int m_targetFps = 30;
    bool m_alwaysOnTop = true;
    bool m_mouseThrough = false;
    bool m_interactionEnabled = true;
    bool m_motionLoopEnabled = true;
    bool m_soundEnabled = true;
    bool m_doubleClickSwitchEnabled = true;
    int m_gazeStrength = 2; // 0=无 1=弱 2=中 3=强
};

} // namespace kanban

#endif // KANBANCONTROLLER_H
