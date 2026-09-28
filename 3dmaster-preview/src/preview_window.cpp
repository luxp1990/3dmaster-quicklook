#include <QtCore/qt_windows.h>
#include "preview_window.h"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QDebug>

PreviewWindow::PreviewWindow(QWidget* parent) : QWidget(parent) {
    qRegisterMetaType<ModelDataPtr>("ModelDataPtr");

    // 无边框子窗口
    setWindowFlags(Qt::FramelessWindowHint | Qt::SubWindow);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);

    // 创建守护进程内部隐藏脱离孤岛窗口 (WS_POPUP, 屏幕外)
    m_daemonIslandHwnd = ::CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"STATIC",
        L"3dmaster_DaemonIsland",
        WS_POPUP,
        -32000, -32000, 10, 10,
        NULL, NULL, NULL, NULL);

    m_widget = new MasterWidget(this);
    m_sidebar = new MasterSidebar(this);

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_widget, 1);
    layout->addWidget(m_sidebar, 0);

    // 侧边栏与视口信号槽对接
    connect(m_sidebar, &MasterSidebar::sigCameraPreset, m_widget, &MasterWidget::switchCamera);
    connect(m_sidebar, &MasterSidebar::sigFitViewRequested, m_widget, &MasterWidget::fitView);
    connect(m_sidebar, &MasterSidebar::sigShadingModeChanged, m_widget, &MasterWidget::setShadingMode);
    connect(m_sidebar, &MasterSidebar::sigShowGrid, m_widget, &MasterWidget::setShowGrid);
    connect(m_sidebar, &MasterSidebar::sigShowAxis, m_widget, &MasterWidget::setShowAxis);
    connect(m_sidebar, &MasterSidebar::sigShowWireframe, m_widget, &MasterWidget::setShowWireframe);
    connect(m_sidebar, &MasterSidebar::sigShowFeatureEdges, m_widget, &MasterWidget::setShowFeatureEdges);
    connect(m_sidebar, &MasterSidebar::sigShowBoundingBox, m_widget, &MasterWidget::setShowBoundingBox);
    connect(m_sidebar, &MasterSidebar::sigOrthographic, m_widget, &MasterWidget::setOrthographic);

    connect(m_sidebar, &MasterSidebar::sigSectionEnabled, m_widget, &MasterWidget::setSectionEnabled);
    connect(m_sidebar, &MasterSidebar::sigSectionAxisChanged, m_widget, &MasterWidget::setSectionAxis);
    connect(m_sidebar, &MasterSidebar::sigSectionDepthChanged, m_widget, &MasterWidget::setSectionDepth);

    connect(m_sidebar, &MasterSidebar::sigPartVisibleChanged, m_widget, &MasterWidget::setPartVisible);
    connect(m_sidebar, &MasterSidebar::sigAllPartsVisibleChanged, m_widget, &MasterWidget::setAllPartsVisible);
}

PreviewWindow::~PreviewWindow() {
    cancelCurrentLoad();
    detachToIsland();
    if (m_daemonIslandHwnd && ::IsWindow(m_daemonIslandHwnd)) {
        ::DestroyWindow(m_daemonIslandHwnd);
        m_daemonIslandHwnd = nullptr;
    }
}

void PreviewWindow::attachTo(HWND parentHwnd) {
    m_parentHwnd = parentHwnd;
    HWND myHwnd = reinterpret_cast<HWND>(winId());

    if (!parentHwnd || !::IsWindow(parentHwnd)) {
        qWarning() << "[PreviewWindow] Invalid parent HWND:" << parentHwnd;
        return;
    }

    ::SetParent(myHwnd, parentHwnd);

    LONG style = ::GetWindowLongW(myHwnd, GWL_STYLE);
    style |= WS_CHILD;
    style &= ~WS_POPUP;
    ::SetWindowLongW(myHwnd, GWL_STYLE, style);

    RECT rc;
    int w = 800, h = 600;
    if (::GetClientRect(parentHwnd, &rc)) {
        int rw = rc.right - rc.left;
        int rh = rc.bottom - rc.top;
        if (rw > 0 && rh > 0) {
            w = rw;
            h = rh;
        }
    }

    ::SetWindowPos(myHwnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    this->resize(w, h);
    this->show();
    this->raise();

    if (m_widget) {
        m_widget->show();
        m_widget->update();
    }
    if (m_sidebar) {
        m_sidebar->show();
    }
}

void PreviewWindow::resizeTo(int w, int h) {
    if (w <= 0 || h <= 0) return;
    HWND myHwnd = reinterpret_cast<HWND>(winId());
    if (m_parentHwnd && ::IsWindow(m_parentHwnd)) {
        ::SetWindowPos(myHwnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    this->resize(w, h);
    if (m_widget) {
        m_widget->update();
    }
}

void PreviewWindow::detachToIsland() {
    HWND myHwnd = reinterpret_cast<HWND>(winId());
    if (m_daemonIslandHwnd && ::IsWindow(m_daemonIslandHwnd)) {
        ::SetParent(myHwnd, m_daemonIslandHwnd);
        ::ShowWindow(myHwnd, SW_HIDE);
    }
    m_parentHwnd = nullptr;
}

bool PreviewWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(eventType);
    MSG* msg = static_cast<MSG*>(message);
    if (msg && msg->message == WM_MOUSEACTIVATE) {
        // 防止鼠标点击 3D 视口抢夺宿主键盘焦点
        *result = MA_NOACTIVATE;
        return true;
    }
    return QWidget::nativeEvent(eventType, message, result);
}

void PreviewWindow::keyPressEvent(QKeyEvent* event) {
    int k = event->key();
    if (k == Qt::Key_Space || k == Qt::Key_Escape ||
        k == Qt::Key_Up || k == Qt::Key_Down ||
        k == Qt::Key_Left || k == Qt::Key_Right) {
        if (m_parentHwnd && ::IsWindow(m_parentHwnd)) {
            UINT vk = (k == Qt::Key_Space) ? VK_SPACE :
                      (k == Qt::Key_Escape) ? VK_ESCAPE :
                      (k == Qt::Key_Up) ? VK_UP :
                      (k == Qt::Key_Down) ? VK_DOWN :
                      (k == Qt::Key_Left) ? VK_LEFT : VK_RIGHT;
            ::PostMessageW(m_parentHwnd, WM_KEYDOWN, vk, 0);
            event->ignore();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void PreviewWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}

void PreviewWindow::cancelCurrentLoad() {
    if (m_worker) {
        m_worker->requestCancel();
    }
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        if (!m_workerThread->wait(1500)) {
            qWarning() << "[PreviewWindow] Worker thread did not finish within 1500ms, detaching";
        }
    }
    m_worker = nullptr;
    m_workerThread = nullptr;
}

void PreviewWindow::clearDoc() {
    cancelCurrentLoad();
    detachToIsland();
    m_currentGenId = 0;
    m_currentPath.clear();
    m_loadedPath.clear();
    m_model.reset();

    if (m_widget) {
        m_widget->setModel(nullptr);
        m_widget->setLoading(false);
    }
    if (m_sidebar) {
        m_sidebar->updateModelStats(nullptr);
    }
}

void PreviewWindow::loadModel(uint64_t genId, const QString& path) {
    qInfo() << "[PreviewWindow] loadModel request: genId=" << genId << "path=" << path;

    // 若已经载入且完全相同
    if (m_model && m_loadedPath == path) {
        m_currentGenId = genId;
        if (m_widget) {
            m_widget->setLoading(false);
        }
        emit sigModelLoadFinished(genId, true, "");
        return;
    }

    cancelCurrentLoad();

    m_currentGenId = genId;
    m_currentPath = path;

    if (m_widget) {
        m_widget->setLoading(true, "正在准备解析 3D 几何数据...");
    }

    m_workerThread = new QThread();
    m_worker = new ModelLoaderWorker(path);
    m_worker->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started, m_worker, &ModelLoaderWorker::startLoading);

    connect(m_worker, &ModelLoaderWorker::sigProgress, this, [this, genId](int pct, const QString& status) {
        if (genId != m_currentGenId) return;
        if (m_widget) {
            m_widget->setLoading(true, QString("%1 (%2%)").arg(status).arg(pct));
        }
    });

    connect(m_worker, &ModelLoaderWorker::sigFinished, this, [this, genId](ModelDataPtr model) {
        if (genId != m_currentGenId) return;
        onModelLoaded(model);
    });

    connect(m_worker, &ModelLoaderWorker::sigFailed, this, [this, genId](const QString& err) {
        if (genId != m_currentGenId) return;
        onModelLoadFailed(err);
    });

    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_workerThread, &QThread::finished, m_workerThread, &QObject::deleteLater);

    m_workerThread->start();
}

void PreviewWindow::onModelLoaded(ModelDataPtr model) {
    m_model = model;
    m_loadedPath = m_currentPath;

    if (m_widget) {
        m_widget->setModel(model);
        m_widget->setLoading(false);
        m_widget->fitView();
    }
    if (m_sidebar) {
        m_sidebar->updateModelStats(model);
    }

    emit sigModelLoadFinished(m_currentGenId, true, "");
}

void PreviewWindow::onModelLoadFailed(const QString& err) {
    qWarning() << "[PreviewWindow] Model load failed:" << err;
    if (m_widget) {
        m_widget->setErrorMessage(err);
        m_widget->setLoading(false);
    }
    emit sigModelLoadFinished(m_currentGenId, false, err);
}
