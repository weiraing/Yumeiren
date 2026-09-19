// GLEW 必须早于其他 GL 头文件。
#include <GL/glew.h>

#include "kanban/CubismRuntime.h"
#include "kanban/KanbanRenderer.h"
#include "core/Diagnostics.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <string>

#include "CubismFramework.hpp"
#include "ICubismAllocator.hpp"
#include "Rendering/CubismRenderer.hpp"
#include "Rendering/OpenGL/CubismShader_OpenGLES2.hpp"

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::Rendering;

namespace kanban::cubismruntime {
namespace {

constexpr char kModule[] = "Live2D";

QString logLine(const QString &text)
{
    return QStringLiteral("[Live2D] %1").arg(text);
}

void cubismLogBridge(const char *message)
{
    if (!message) {
        return;
    }
    const QString text = QString::fromUtf8(message).trimmed();
    if (text.isEmpty()) {
        return;
    }
    // SDK 仅提供文本回调，从前缀还原日志级别。
    videodiag::Level level = videodiag::Level::Debug;
    if (text.contains(QStringLiteral("[error]"), Qt::CaseInsensitive)) {
        level = videodiag::Level::Error;
    } else if (text.contains(QStringLiteral("[warning]"), Qt::CaseInsensitive)) {
        level = videodiag::Level::Warning;
    } else if (text.contains(QStringLiteral("[info]"), Qt::CaseInsensitive)) {
        level = videodiag::Level::Info;
    }
    videodiag::log(level, logLine(text), QLatin1String(kModule));
}

class CubismAllocator final : public ICubismAllocator
{
public:
    void *Allocate(const csmSizeType size) override
    {
        return std::malloc(static_cast<size_t>(size));
    }
    void Deallocate(void *memory) override
    {
        std::free(memory);
    }
    void *AllocateAligned(const csmSizeType size, const csmUint32 alignment) override
    {
        const size_t align = alignment > 0 ? alignment : alignof(std::max_align_t);
        return _aligned_malloc(static_cast<size_t>(size), align);
    }
    void DeallocateAligned(void *alignedMemory) override
    {
        _aligned_free(alignedMemory);
    }
};

// StartUp 只保存指针，分配器和选项必须与进程同寿；框架不随渲染器销毁。
CubismAllocator g_allocator;
CubismFramework::Option g_option;
quint64 g_shaderCacheGeneration = 0;

// 着色器优先从程序目录读取，避免快捷方式启动时工作目录变化。
csmByte *loadCubismFileBytes(const std::string filePath, csmSizeInt *outSize)
{
    if (outSize) {
        *outSize = 0;
    }
    const QString relative = QString::fromUtf8(filePath.c_str());
    if (relative.isEmpty()) {
        return nullptr;
    }

    QStringList candidates;
    if (QFileInfo(relative).isAbsolute()) {
        candidates << relative;
    } else {
        candidates << QDir(QCoreApplication::applicationDirPath()).filePath(relative)
                   << QDir::current().filePath(relative);
    }

    for (const QString &path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray data = file.readAll();
        if (file.error() != QFileDevice::NoError || data.isEmpty()
            || data.size() > (std::numeric_limits<csmSizeInt>::max)()) {
            continue;
        }
        // SDK 按长度读取，不需要终止符；由 releaseCubismFileBytes 配对释放。
        auto *bytes = new csmByte[static_cast<size_t>(data.size())];
        std::memcpy(bytes, data.constData(), static_cast<size_t>(data.size()));
        if (outSize) {
            *outSize = static_cast<csmSizeInt>(data.size());
        }
        return bytes;
    }

    logWarn(QStringLiteral("读不到运行期资源：%1(程序目录 %2)")
                .arg(relative, QCoreApplication::applicationDirPath()));
    return nullptr;
}

void releaseCubismFileBytes(csmByte *byteData)
{
    delete[] byteData;
}

void resetGlobalGlState()
{
    // 仅在缓存已存在时调用，避免 GetInstance 意外创建并编译着色器。顺序硬要求：先清 CPU 侧记录(否则析构会向已销毁的上下文发 glDeleteProgram)，再销毁单例并重置 GL 入口。
    CubismShader_OpenGLES2::GetInstance()->ReleaseInvalidShaderProgram();
    CubismRenderer::StaticRelease();
}

} // namespace

void logInfo(const QString &text)
{
    videodiag::log(videodiag::Level::Info, logLine(text), QLatin1String(kModule));
}

void logWarn(const QString &text)
{
    videodiag::log(videodiag::Level::Warning, logLine(text), QLatin1String(kModule));
}

void logDebug(const QString &text)
{
    videodiag::log(videodiag::Level::Debug, logLine(text), QLatin1String(kModule));
}

bool initializeFramework(QString *outError)
{
    // 日志和读盘回调必须早于 StartUp，才能记录 Core 版本并加载首帧着色器。
    g_option.LogFunction = &cubismLogBridge;
    g_option.LoggingLevel = CubismFramework::Option::LogLevel_Warning;
    g_option.LoadFileFunction = &loadCubismFileBytes;
    g_option.ReleaseBytesFunction = &releaseCubismFileBytes;

    if (!CubismFramework::IsStarted() && !CubismFramework::StartUp(&g_allocator, &g_option)) {
        const QString reason = QStringLiteral("CubismFramework::StartUp 失败");
        if (outError) {
            *outError = reason;
        }
        logWarn(reason);
        return false;
    }
    if (!CubismFramework::IsInitialized()) {
        CubismFramework::Initialize();
    }
    logInfo(QStringLiteral("框架就绪，Core 版本 0x%1")
                .arg(static_cast<quint32>(Live2D::Cubism::Core::csmGetVersion()),
                     8, 16, QLatin1Char('0')));
    return true;
}

void syncShaderCache(quint64 generation)
{
    if (generation == 0) {
        return;
    }
    // 着色器是进程级单例，但 program id 属于上下文：换代后必须重建。
    if (g_shaderCacheGeneration != 0 && g_shaderCacheGeneration != generation) {
        logInfo(QStringLiteral("GL 上下文已换代(%1 → %2)，丢弃 Cubism 着色器缓存并重建")
                    .arg(g_shaderCacheGeneration)
                    .arg(generation));
        resetGlobalGlState();
    }
    g_shaderCacheGeneration = generation;
}

GlScope::GlScope(KanbanGlHost *host, bool *glewReady)
    : m_host(host)
{
    if (!m_host || !m_host->glHostReady()) {
        return;
    }
    if (!m_host->glIsCurrent()) {
        if (!m_host->glMakeCurrent()) {
            return;
        }
        m_owned = true;
    }
    // 纹理上传早于首帧渲染，必须在首次 GL 调用前初始化 GLEW；换上下文后重置标志。
    if (glewReady && !*glewReady) {
        if (glewInit() != GLEW_OK) {
            logWarn(QStringLiteral("glewInit 失败，GL 函数表不可用"));
            return;
        }
        *glewReady = true;
    }
    m_ok = true;
}

GlScope::~GlScope()
{
    if (m_owned) {
        m_host->glDoneCurrent();
    }
}

} // namespace kanban::cubismruntime
