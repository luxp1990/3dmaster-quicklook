#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include <QtCore/qt_windows.h>

#include "preview_window.h"
#include "ipc_server.h"

int main(int argc, char* argv[]) {
    // 启用高分屏缩放自适应
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("3dmaster-preview");
    app.setOrganizationName("3dmaster");

    QCommandLineParser parser;
    parser.setApplicationDescription("3dmaster QuickLook Standalone Preview Daemon");
    parser.addHelpOption();

    QCommandLineOption pipeOption(QStringList() << "p" << "pipe",
        "Named pipe name for IPC communication.", "pipeName", "3dmaster_quicklook");
    parser.addOption(pipeOption);

    QCommandLineOption parentHwndOption(QStringList() << "parent",
        "Initial parent HWND to embed into.", "parentHwnd");
    parser.addOption(parentHwndOption);

    QCommandLineOption idleTimeoutOption(QStringList() << "idle-timeout",
        "Idle timeout in seconds before auto-quitting (0 to disable, default: 600s / 10min).", "seconds", "600");
    parser.addOption(idleTimeoutOption);

    parser.process(app);

    QString pipeName = parser.value(pipeOption);
    if (pipeName.isEmpty()) {
        pipeName = "3dmaster_quicklook";
    }

    int idleTimeoutSec = parser.value(idleTimeoutOption).toInt();
    if (idleTimeoutSec <= 0) {
        idleTimeoutSec = 600; // 默认 10 分钟
    }

    qInfo() << "[3dmaster-preview] Starting with pipeName:" << pipeName << "idleTimeout:" << idleTimeoutSec << "s";

    PreviewWindow window;
    IpcServer ipcServer(pipeName);

    // 活跃状态与附着跟踪
    auto lastActivityTime = std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch());
    auto isAttached = std::make_shared<bool>(false);

    auto recordActivity = [lastActivityTime]() {
        *lastActivityTime = QDateTime::currentMSecsSinceEpoch();
    };

    QObject::connect(&ipcServer, &IpcServer::sigActivity, recordActivity);

    // 绑定 IPC 事件与视窗动作
    QObject::connect(&ipcServer, &IpcServer::sigAttachHwnd, &window, [&window, &ipcServer, isAttached, recordActivity](quint64 hwndVal) {
        HWND parentHwnd = reinterpret_cast<HWND>(hwndVal);
        window.attachTo(parentHwnd);
        *isAttached = (parentHwnd != nullptr);
        recordActivity();
        ipcServer.sendAttached(true);
    });

    QObject::connect(&ipcServer, &IpcServer::sigLoadModel, &window, [&window, recordActivity](uint64_t genId, const QString& path) {
        recordActivity();
        window.loadModel(genId, path);
    });

    QObject::connect(&ipcServer, &IpcServer::sigResize, &window, [&window, recordActivity](int w, int h) {
        recordActivity();
        window.resizeTo(w, h);
    });

    QObject::connect(&ipcServer, &IpcServer::sigClearDoc, &window, [&window, isAttached, recordActivity]() {
        recordActivity();
        *isAttached = false;
        window.clearDoc();
    });

    QObject::connect(&ipcServer, &IpcServer::sigQuit, &app, &QApplication::quit);

    QObject::connect(&window, &PreviewWindow::sigModelLoadFinished, &ipcServer, [&ipcServer, recordActivity](uint64_t genId, bool ok, const QString& err) {
        recordActivity();
        ipcServer.sendLoaded(genId, ok, err);
    });

    // 启动 2 秒周期定时器，检测无附着状态下的空闲超时自退
    QTimer idleTimer;
    QObject::connect(&idleTimer, &QTimer::timeout, [&app, lastActivityTime, isAttached, idleTimeoutSec]() {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 elapsedSec = (now - *lastActivityTime) / 1000;

        // 仅在无客户端附着（未 ATTACH 或已 CLEAR）且超时时退出
        if (!(*isAttached) && elapsedSec >= idleTimeoutSec) {
            qInfo() << "[3dmaster-preview] Idle timeout reached (" << elapsedSec << "s >= " << idleTimeoutSec << "s with no active client). Quitting cleanly.";
            app.quit();
        }
    });
    idleTimer.start(2000);

    if (!ipcServer.start()) {
        qCritical() << "[3dmaster-preview] Failed to start IPC Server on pipe:" << pipeName;
        return -1;
    }

    // 若命令行传入了初始 parent
    if (parser.isSet(parentHwndOption)) {
        bool ok = false;
        quint64 parentVal = parser.value(parentHwndOption).toULongLong(&ok, 0);
        if (ok && parentVal != 0) {
            window.attachTo(reinterpret_cast<HWND>(parentVal));
            *isAttached = true;
            recordActivity();
        }
    }

    return app.exec();
}
