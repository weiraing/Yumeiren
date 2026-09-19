// 看板娘控制器：拥有窗口与渲染器，窗口只借用渲染器。
// 运行状态由状态机管理，动画仅由统一时钟推进；Live2D 失败时降级为占位后端。
#ifndef KANBANCONTROLLER_H
#define KANBANCONTROLLER_H

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
    // 返回后端实际支持的表情数，界面据此启用入口。
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
    void playNext();    // 下一个动作；没有可播动作就到此为止(不换模型)
    void playNextExpression(); // 下一个表情；没有表情就静默返回(不换模型)
    // 可播动作数(不含 idle)与「这个入口该不该可点」。界面拿它置灰，
    // 判据收在渲染器基类里一处，三条入口(菜单/设置页/托盘)共用。
    int playableMotionCount() const;
    bool canPlayNextMotion() const;
    // 只有 start() 会调用(软件渲染后端启动的最后一步)；GL 后端在等上下文时
    // 就已经 show 过窗口。曾有配对的 hideWindow()，「暂时隐藏」删除后一并去掉了。
    void showWindow();

    // —— 设置(全部即时生效并落盘) ——
    void setScalePercent(int percent);
    int scalePercent() const { return m_scalePercent; }
    void setOpacityPercent(int percent);
    int opacityPercent() const { return m_opacityPercent; }
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
    // 「有没有开」= 档位 > 0。做成一处判据，免得各调用点自己写 `!= 0`。
    bool gazeTracking() const { return m_gazeStrength != 0; }
    bool setModelPath(const QString &modelJsonPath);
    QString modelPath() const { return m_modelPath; }
    int refreshModels(); // 重新扫描模型目录，返回可用模型数

    // 显示器电源状态(由主窗口的 WM_POWERBROADCAST 投递，与视频壁纸同一个事件源)。
    // 熄屏期间桌面上没有任何东西需要绘制，看板娘却还在按帧率整帧画进 GL。
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
    void openSettingsRequested(); // 右键菜单「打开主界面设置」
    void quitKanbanRequested();

private slots:
    void onFrameTick(float deltaSeconds);
    void onGlContextReady();
    // 挂起判定心跳(1s)。判据与节奏跟视频壁纸同一套，但各自独立跑：看板娘可以
    // 在壁纸停着的情况下单独运行，反过来也一样。
    void evaluateSuspend();

private:
    // 挂起原因位。刻意只有这两项：
    //   · 锁屏/熄屏是**必然看不见**，且判据本机可实测(会话锁定查询 + 电源广播)；
    //   · 「前台全屏 / 桌面被遮挡」不做：看板娘默认置顶，本来就画在全屏应用之上；
    //     不置顶时的遮挡判定又依赖多屏语义，本机单屏验证不了(视频壁纸那边同一个
    //     结论，见 VideoWallpaper::evaluateSuspend 的注释)。宁可不省也不猜。
    //
    // 也刻意**不看主窗口有没有收进托盘**：看板娘是独立的桌面窗口，主界面隐藏时它
    // 仍露在桌面上，冻住它只会看起来像坏了(2026-09-17 已按用户要求删掉那条联动)。
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
    QElapsedTimer *m_suspendClock = nullptr; // 本次持续挂起的时长
    int m_suspendReasons = 0;
    bool m_releasedForSuspend = false;
    bool m_monitorOn = true;
    // 假挂起窗口的起算表(见 fakeSuspendReasons)，只在设了环境变量时走动。
    QElapsedTimer m_fakeSuspendClock;

    // 设置镜像(落盘的单一来源；改设置走 setter，别直接写这几个字段)
    int m_scalePercent = 100;
    int m_opacityPercent = 100;
    int m_targetFps = 30;
    bool m_alwaysOnTop = true;
    bool m_mouseThrough = false;
    bool m_interactionEnabled = true;
    // 视线追踪档位(0=无 1=弱 2=中 3=强)。>0 即在喂目标。
    int m_gazeStrength = 2;
};

} // namespace kanban

#endif // KANBANCONTROLLER_H
