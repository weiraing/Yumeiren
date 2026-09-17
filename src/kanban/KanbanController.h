// 看板娘控制器：状态机 + 统一时钟 + 渲染器 + 模型表 + 窗口的唯一组装者。
//
// 所有权(任务书 §4.1)：控制器拥有渲染器与窗口，窗口只「引用」渲染器；
// 换后端 = 换 renderer 指针 + 重新 attach，窗口与配置都不动。
//
// 三条设计约束：
//   · 看板娘失败绝不能拖垮主程序：start() 返回 bool，Live2D 不可用自动降级到
//     内置占位动画，只有两者都起不来才进 Error；
//   · 状态判定只在状态机一处：控制器不存 isStarted/isPausing 布尔组合；
//   · 一律不自己 new QTimer 做动画：动画推进唯一来源是 KanbanAnimationClock。
#ifndef KANBANCONTROLLER_H
#define KANBANCONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

#include "kanban/KanbanModelManager.h"
#include "kanban/KanbanStateMachine.h"
#include "kanban/KanbanTypes.h"

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
    // 当前渲染器的表情数(0 = 没有表情)。界面据此决定「切换表情」入口是否可用；
    // 问的是渲染器而不是模型表，因为真正能不能切由后端说了算。
    int expressionCount() const;
    // 界面下拉框用：有效模型的完整信息(名称/路径/贴图与动作计数)，顺序与
    // modelNames() 一致。只给名字不够 —— 用户要能看出哪个模型是「空壳」。
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
    void showWindow();  // 暂时隐藏后重新显示(不重启)
    void hideWindow();  // 隐藏但保持已装载状态与运行语义

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
    void setAutoStart(bool on);
    bool autoStart() const;
    void setPauseWhenMainHidden(bool on);
    bool pauseWhenMainHidden() const;
    bool setModelPath(const QString &modelJsonPath);
    QString modelPath() const { return m_modelPath; }
    int refreshModels(); // 重新扫描模型目录，返回可用模型数

    void loadSettings(); // 从配置读回全部看板娘设置
    void applyMainWindowVisible(bool visible); // 主窗口显示/隐藏联动(§7.7)
    void shutdownForExit();                    // 退出收口：不留 GL 资源与定时器

signals:
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

private:
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

    // 设置镜像(落盘的单一来源；改设置走 setter，别直接写这几个字段)
    int m_scalePercent = 100;
    int m_opacityPercent = 100;
    int m_targetFps = 30;
    bool m_alwaysOnTop = true;
    bool m_mouseThrough = false;
    bool m_interactionEnabled = true;
};

} // namespace kanban

#endif // KANBANCONTROLLER_H
