#include "SdlVideoWidget.h"
#include "VideoState.h"
#include "Player.h"
#include <QResizeEvent>
#include <QMouseEvent>
#include <QPointF>

SdlVideoWidget::SdlVideoWidget(QWidget* parent)
    : QWidget(parent)
{
    // WA_NativeWindow: 确保控件拥有独立的 X11 原生窗口，SDL 可直接渲染
    // WA_OpaquePaintEvent: Qt 不绘制背景
    // paintEngine() 返回 nullptr: 禁用 Qt 绘制引擎
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);

    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, &SdlVideoWidget::onRenderTick);
}

SdlVideoWidget::~SdlVideoWidget() 
{
    stopRendering();
}

void SdlVideoWidget::attachVideoState(VideoState* is)
{
    m_videoState = is;
    if (!is) return;

    // ---- 清理旧的 SDL 资源 ----
    if (is->window) {
        SDL_DestroyWindow(is->window);
        is->window = nullptr;
    }
    if (is->renderer) {
        SDL_DestroyRenderer(is->renderer);
        is->renderer = nullptr;
    }

    // ---- SDL 窗口嵌入到 Qt 控件 ----
    // 前提：Qt 必须使用 XCB 后端（main.cpp 中 qputenv("QT_QPA_PLATFORM", "xcb")）
    // 这样 winId() 返回的是合法 X11 窗口句柄

    // 强制创建原生窗口
    createWinId();

    // SDL 包装已有的 X11 窗口（不创建新窗口）
    is->window = SDL_CreateWindowFrom((void*)winId());
    if (!is->window) {
        fprintf(stderr, "SDL_CreateWindowFrom failed: %s\n", SDL_GetError());
        return;
    }

    // 创建渲染器（软件渲染，SDL_CreateWindowFrom 的窗口可能不支持硬件加速）
    is->renderer = SDL_CreateRenderer(is->window, -1, SDL_RENDERER_SOFTWARE);
    if (!is->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(is->window);
        is->window = nullptr;
        return;
    }

    // 同步窗口大小
    SDL_SetWindowSize(is->window, width(), height());
}
// ==================== 渲染控制 ====================
void SdlVideoWidget::startRendering(int intervalMs)
{
    if (m_rendering) return;
    m_rendering = true;
    m_renderTimer->start(intervalMs);
}
//停止渲染
void SdlVideoWidget::stopRendering()
{
    m_rendering = false;
    m_renderTimer->stop();
}
//==================== 渲染槽函数 ====================
void SdlVideoWidget::onRenderTick()
{
    if (!m_videoState || m_videoState->quit) return;

    // 消费所有 SDL 事件（必须用 PollEvent 而非 PumpEvents）：
    // SDL_PumpEvents 只读取原始事件放入队列，不处理 WM 协议响应。
    // SDL_PollEvent 才会真正消费事件并自动回复 _NET_WM_PING，
    // 否则窗口管理器检测到窗口 5 秒无 ping 回复 → ANR 对话框。
    SDL_Event dummy;
    while (SDL_PollEvent(&dummy)) {
        // 只需消费事件，SDL 内部自动处理 WM ping 回复
    }

    if (!m_videoState->pause) {
        render_frame(m_videoState);
    }
}

// ==================== Qt 事件 → 坐标转发 ====================

QPointF SdlVideoWidget::toNormalized(int px, int py) const
{
    return QPointF(
        (double)px / std::max(1, width()),
        (double)py / std::max(1, height())
    );
}

void SdlVideoWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        QPointF n = toNormalized(event->x(), event->y());
        m_dragging = true;
        emit videoMousePressed(n.x(), n.y());
    }
}

void SdlVideoWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        QPointF n = toNormalized(event->x(), event->y());       // 归一化坐标
        emit videoMouseMoved(n.x(), n.y());
    }
    // 可选：鼠标在非拖动时也能检测是否在进度条区域
    QPointF n = toNormalized(event->x(), event->y());
    // 可以 emit 一个 hover 信号给 MainWindow 用于显示光标样式
}

void SdlVideoWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        QPointF n = toNormalized(event->x(), event->y());
        m_dragging = false;
        emit videoMouseReleased(n.x(), n.y());
    }
}

void SdlVideoWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // SDL 窗口跟随 Qt Widget 大小
    if (m_videoState && m_videoState->window) {
        SDL_SetWindowSize(m_videoState->window, event->size().width(), event->size().height());
    }
}
