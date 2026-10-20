#pragma once

#include <QWidget>
#include <QTimer>
#include <QPaintEvent>
extern "C" {
    #include <SDL2/SDL.h>
}

class VideoState;

/**
 * 将 SDL2 窗口嵌入到 Qt Widget 中
 * 
 * 原理：SDL_CreateWindowFrom((void*)winId()) 把 SDL 渲染目标绑定到
 * Qt 控件的原生窗口句柄上，这样就实现了"Qt 管布局，SDL 管渲染"。
 */
class SdlVideoWidget : public QWidget {
    Q_OBJECT

public:
    explicit SdlVideoWidget(QWidget* parent = nullptr);
    ~SdlVideoWidget() override;

    /** 必须重写：WA_PaintOnScreen 下返回 nullptr，消除 paintEngine 警告 */
    QPaintEngine* paintEngine() const override { return nullptr; }

    /** 将 VideoState 中的 SDL 组件（window/renderer/texture）绑定到此 Widget */
    void attachVideoState(VideoState* is);

    /** 启动/停止渲染定时器 */
    void startRendering(int intervalMs = 16);   // 16ms ≈ 60fps
    void stopRendering();

    bool isRendering() const { return m_rendering; }

signals:
    /** 鼠标在视频区域按下（带归一化坐标，用于进度条拖拽） */
    void videoMousePressed(double normalizedX, double normalizedY);
    void videoMouseMoved(double normalizedX, double normalizedY);
    void videoMouseReleased(double normalizedX, double normalizedY);

protected:
    // Qt 事件处理
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override {}  // SDL 管理绘制，Qt 不需要画任何东西
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    void onRenderTick();

private:
    VideoState* m_videoState = nullptr;
    QTimer*     m_renderTimer = nullptr;
    bool        m_rendering = false;
    bool        m_dragging = false;

    /** 将像素坐标转为 [0,1] 归一化坐标 */
    QPointF toNormalized(int px, int py) const;
};
