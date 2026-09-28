#pragma once

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <cstdint>

class IpcServer : public QObject {
    Q_OBJECT
public:
    explicit IpcServer(const QString& pipeName, QObject* parent = nullptr);
    ~IpcServer() override;

    bool start();
    void stop();

    void sendAttached(bool ok);
    void sendLoaded(uint64_t genId, bool ok, const QString& errorMsg = QString());
    void sendCleared();
    void sendPong();

signals:
    void sigAttachHwnd(quint64 hwnd);
    void sigLoadModel(uint64_t genId, const QString& path);
    void sigResize(int width, int height);
    void sigClearDoc();
    void sigQuit();
    void sigActivity();

private slots:
    void onNewConnection();
    void onSocketDisconnected();
    void onReadyRead();

private:
    void processCommandLine(const QString& line);

private:
    QString m_pipeName;
    QLocalServer* m_server = nullptr;
    QLocalSocket* m_clientSocket = nullptr;
    QByteArray m_buffer;
};
