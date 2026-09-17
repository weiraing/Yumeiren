#include "kanban/ModelThumbJob.h"

#include "kanban/KanbanModelManager.h"
#include "kanban/ModelThumbCache.h"

#include <QString>

// 非 Live2D 构建(没有 Cubism SDK / 没有 Qt6::OpenGL)时整个文件退化成一句话。
// 用条件编译而不是拆成两个文件：这段逻辑只有一个「能不能做」的分支，拆开反而
// 要在 CMakeLists 里再维护一条二选一，收益不抵成本。
#if !defined(YUMEIREN_WITH_LIVE2D)

namespace kanban {

int runModelThumbJob(const QStringList &args, QString *error)
{
    Q_UNUSED(args)
    if (error)
        *error = QStringLiteral("本构建未编译 Live2D 后端，无法生成模型预览图。");
    return 2;
}

} // namespace kanban

#else

#include "kanban/KanbanRenderer.h"
#include "kanban/Live2DRenderer.h"
#include "kanban/OffscreenGlHost.h"

#include <QCoreApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QVector>

#include <cstdio>

namespace kanban {

namespace {

// 预览图的绘制面**逻辑**尺寸。
//
// 取 320×480 而不是随便一个方图：这是看板娘窗口在 100% 缩放下的逻辑尺寸
// (Live2DRenderer 的默认绘制面)。同尺寸 = 同构图，格子里看到的就是把模型放上
// 桌面会得到的样子，用户不会觉得「预览和实际不一样」。
constexpr int kThumbLogicalWidth = 320;
constexpr int kThumbLogicalHeight = 480;

// 超采样倍数：实际渲染面 = 逻辑尺寸 × 这个数。
//
// 为什么不取 1：模型在画面里占多大由它自己的 Layout 决定，实测同一个模型目录里
// 裁掉透明边之后差别极大 —— 小的只剩 78×186(如 Rice)，大的能铺满整个 320×480。
// 而格子在 150% 缩放下要 158×236 个物理像素，78 宽的那张等于要放大 2 倍，
// 糊得肉眼可见。统一按 2 倍渲染，所有模型就都落在「下采样」这一侧，格子里
// 始终是锐的，高 DPI 屏也扛得住。
//
// 代价是一次性的，且很小：渲染像素数翻两番对 GPU 无感(整批 16 张仍是几秒)，
// 缓存总量从 ~1.3MB 变成 ~4MB。用户说的「省资源」指的是别反复重渲染，
// 不是省这几百 KB 磁盘 —— 图只出一次，之后一直命中缓存。
constexpr int kThumbSupersample = 2;

constexpr int kThumbWidth = kThumbLogicalWidth * kThumbSupersample;
constexpr int kThumbHeight = kThumbLogicalHeight * kThumbSupersample;

// 抓图前推进的帧数。
//
// 一帧只能验证「画出来了」：模型刚装载时姿势、物理、待机动作都还在初始态，
// 直接抓会得到一个僵硬的立正姿势。推 30 帧(约 0.5 秒)让待机动作与物理摆到
// 自然状态，抓到的才是「这个模型平时长什么样」。
constexpr int kSettleFrames = 30;

// 裁到内容边界时四周留的透明边(像素)。
// 留一点而不是贴着画：模型边缘(头发、饰品)常有半透明抗锯齿像素，贴边裁会显得挤。
constexpr int kTrimMargin = 6;

// 产品里 QOpenGLWidget 用的那套表面格式，这里照抄 —— 两边格式不同的话，
// 「探针/生成器能出图」就证明不了「产品能出图」。
QSurfaceFormat thumbSurfaceFormat()
{
    QSurfaceFormat format;
    format.setAlphaBufferSize(8);
    format.setSamples(0);
    format.setMajorVersion(3);
    format.setMinorVersion(3);
    format.setProfile(QSurfaceFormat::NoProfile);
    return format;
}

// 往 stdout 打一行进度。**仅供人在控制台手工跑时查看**，不是与主程序的协议。
//
// 为什么不能当协议：本程序是 **GUI 子系统**的可执行文件(WIN32 目标)，实测它的
// stdout 只在 GL 上下文建立**之前**写得出去，之后就静默丢失。证据是一次
// 「1 个要渲染 + 15 个跳过」的运行：15 行 SKIP 全在，而随后的 OK 与 DONE
// 一行不剩 —— 图片本身是正常落盘的，说明文件 I/O 没问题，丢的是标准输出。
// 换成 CRT 的 fwrite+fflush 重测**同样丢**，所以不是 QTextStream 的问题，
// 是子系统层面的。同源码的控制台程序 KanbanProbe 没有这个现象。
//
// 因此主程序完全不读它：完成与否只看 <程序目录>/.cache/model-thumbs/ 下的
// 文件(见 ModelThumbJob.h 的契约说明)。这里的 fwrite 只是省得为了几行调试
// 输出引一个 QTextStream，顺带保证在能出字的环境里逐行即时可见。
void report(const QString &line)
{
    const QByteArray bytes = line.toUtf8() + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

QString failLine(const QString &id, const QString &reason)
{
    // 原因里可能有换行(渲染器给的多是带换行的诊断文本)，压成一行，
    // 否则主程序的按行解析会把后半截当成一条独立消息。
    QString flat = reason;
    flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
    flat.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return QStringLiteral("FAIL %1 %2").arg(id, flat.trimmed());
}

// 裁掉四周全透明的边，只留一点留白。整张全透明(什么都没画出来)时返回空 QImage。
//
// 为什么必须裁：预览图是按 320×480(看板娘窗口的比例)渲染的，而模型在画面里占
// 多大由它自己的布局决定 —— 实测同一个模型目录里，有的几乎铺满画面，有的只占
// 中间一小块。不裁的话所有格子一样大、角色大小却差一倍，小的那个根本看不清。
// 裁到内容边界，等于让每个模型都把格子用满。
//
// 顺带成了「静默不出图」的判据：Cubism 最典型的故障是装载、纹理上传、DrawFrame
// 全部正常但画面全空，除了 glGetError 什么都不报。全透明直接判失败，界面上会
// 显示占位图并报「N 个模型没能出图」，比留下一格空白好得多。
QImage trimToContent(const QImage &image, int margin)
{
    int minX = image.width();
    int minY = image.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            if (line[x * 4 + 3] == 0)
                continue;
            if (x < minX)
                minX = x;
            if (x > maxX)
                maxX = x;
            if (y < minY)
                minY = y;
            if (y > maxY)
                maxY = y;
        }
    }
    if (maxX < minX || maxY < minY)
        return QImage(); // 一个不透明像素都没有
    const QRect box =
        QRect(minX - margin, minY - margin,
              maxX - minX + 1 + margin * 2, maxY - minY + 1 + margin * 2)
            .intersected(image.rect());
    return image.copy(box);
}

} // namespace

int runModelThumbJob(const QStringList &args, QString *error)
{
    const auto bail = [error](int code, const QString &reason) {
        if (error)
            *error = reason;
        report(QStringLiteral("DONE 0 0 0"));
        return code;
    };

    if (!Live2DRenderer::sdkCompiledIn())
        return bail(2, QStringLiteral("本构建未编译 Live2D 后端，无法生成模型预览图。"));

    const bool force = args.contains(QStringLiteral("--force"));

    KanbanModelManager manager;
    manager.rescan();

    // 先算清楚「到底要出哪几张图」。
    // 这一步在创建 GL 上下文**之前**做完是有意的：全部命中缓存时(最常见的情况，
    // 只有第一次进页面才需要真出图)，本进程连一个 GL 上下文都不该建。
    struct Task {
        QString id;
        QString jsonPath;
    };
    QVector<Task> tasks;
    int skipped = 0;
    const QVector<ModelInfo> models = manager.models();
    for (const ModelInfo &model : models) {
        // 坏模型在扫描阶段就被标掉了，界面也不会列它，这里不重复报。
        if (!model.valid)
            continue;
        if (!force && ModelThumbCache::has(model.id)) {
            report(QStringLiteral("SKIP %1").arg(model.id));
            ++skipped;
            continue;
        }
        tasks.append({model.id, model.modelJsonPath});
    }

    if (tasks.isEmpty()) {
        report(QStringLiteral("DONE 0 0 %1").arg(skipped));
        return 0;
    }

    // —— 一套 GL 环境服务整批模型 ——
    //
    // 必须共用：Cubism 的着色器缓存是进程级单例，一套上下文编出来的 program id
    // 只在这一套上下文里有效。每换一个上下文就得丢缓存重编(~800ms)，十几个模型
    // 就是十几秒白等。
    QOpenGLContext context;
    context.setFormat(thumbSurfaceFormat());
    if (!context.create())
        return bail(3, QStringLiteral("QOpenGLContext::create 失败。"));

    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid())
        return bail(3, QStringLiteral("QOffscreenSurface 无效。"));

    if (!context.makeCurrent(&surface))
        return bail(3, QStringLiteral("离屏目标 makeCurrent 失败。"));

    // 离屏表面自带的默认帧缓冲尺寸由驱动决定，未必等于我们要的绘制面；
    // 显式建 FBO，「画在哪」和「从哪读」才是同一块、尺寸确定的地方。
    // 注意必须在 makeCurrent 之后构造 —— FBO 的构造函数要抓当前上下文。
    QOpenGLFramebufferObject fbo(kThumbWidth, kThumbHeight,
                                 QOpenGLFramebufferObject::CombinedDepthStencil);
    if (!fbo.isValid())
        return bail(3, QStringLiteral("离屏 FBO 创建失败(%1x%2)。")
                           .arg(kThumbWidth)
                           .arg(kThumbHeight));
    fbo.bind();

    OffscreenGlHost host(&context, &surface, QSize(kThumbWidth, kThumbHeight));
    Live2DRenderer renderer;
    renderer.setGlHost(&host);

    QString rendererError;
    if (!renderer.initialize(&rendererError))
        return bail(3, QStringLiteral("渲染器 initialize 失败：%1").arg(rendererError));

    // 预览图是「静态效果展示」，不要它朝着某个方向歪头。渲染器默认档位是「中」，
    // 不显式关掉的话，模型会按上一次的目标角度摆姿势(惯性还没走完)。
    renderer.setGazeStrength(KanbanRenderer::GazeOff);

    // 待机动作固定取第 0 个。产品运行时是随机挑的(用户盯着看，每次同一段会腻)，
    // 但封面图必须可复现：不同待机动作的取景能差到「大头特写」与「全身站立」，
    // 随机挑会让点一次刷新就换一张封面，还常常正好看不到全貌。
    renderer.setDeterministicIdle(true);

    QOpenGLFunctions *f = context.functions();
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);

    int ok = 0;
    int failed = 0;
    for (const Task &task : tasks) {
        QString error;
        if (!renderer.loadModel(task.jsonPath, &error)) {
            report(failLine(task.id, error.isEmpty() ? QStringLiteral("模型装载失败") : error));
            ++failed;
            continue;
        }
        renderer.resize(kThumbWidth, kThumbHeight, 1.0f);

        // 推进若干帧让待机动作与物理稳定，然后读回。
        for (int i = 0; i < kSettleFrames; ++i) {
            renderer.update(1.0f / 60.0f);
            renderer.render();
        }

        QImage frame(kThumbWidth, kThumbHeight, QImage::Format_RGBA8888);
        f->glReadPixels(0, 0, kThumbWidth, kThumbHeight, GL_RGBA, GL_UNSIGNED_BYTE, frame.bits());
        // GL 原点在左下，QImage 在左上。
        frame = frame.mirrored(false, true);

        // 裁到内容边界：让每个模型都把格子用满；整张全透明则判为出图失败。
        frame = trimToContent(frame, kTrimMargin);
        if (frame.isNull()) {
            report(failLine(task.id, QStringLiteral("画面全空 —— 模型没画出来")));
            ++failed;
            continue;
        }

        const QString saved = ModelThumbCache::store(task.id, frame);
        if (saved.isEmpty()) {
            report(failLine(task.id, QStringLiteral("写不出 PNG")));
            ++failed;
            continue;
        }
        report(QStringLiteral("OK %1").arg(task.id));
        ++ok;
    }

    renderer.shutdown();
    context.doneCurrent();

    report(QStringLiteral("DONE %1 %2 %3").arg(ok).arg(failed).arg(skipped));
    return failed == 0 ? 0 : 1;
}

} // namespace kanban

#endif // YUMEIREN_WITH_LIVE2D
