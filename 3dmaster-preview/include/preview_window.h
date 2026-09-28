#pragma once

#include <QtCore/qt_windows.h>
#include <cstdint>

#include "master_widget.h"
#include "master_sidebar.h"
#include "model_loader_worker.h"

#include <QWidget>
#include <QPointer>
#include <QThread>

class PreviewWindow : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWindow(QWidget* parent = nullptr);
    ~PreviewWindow() override;

    void attachTo(HWND parentHwnd);
    void resizeTo(int w, int h);
    void detachToIsland();
    void loadModel(uint64_t genId, const QString& path);
    void clearDoc();

signals:
    void sigModelLoadFinished(uint64_t genId, bool ok, const QString& errorMsg);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onModelLoaded(ModelDataPtr model);
    void onModelLoadFailed(const QString& err);
    void cancelCurrentLoad();

private:
    MasterWidget* m_widget = nullptr;
    MasterSidebar* m_sidebar = nullptr;

    HWND m_parentHwnd = nullptr;
    HWND m_daemonIslandHwnd = nullptr;
    uint64_t m_currentGenId = 0;
    QString m_currentPath;
    QString m_loadedPath;
    ModelDataPtr m_model;

    QPointer<QThread> m_workerThread;
    QPointer<ModelLoaderWorker> m_worker;
};
