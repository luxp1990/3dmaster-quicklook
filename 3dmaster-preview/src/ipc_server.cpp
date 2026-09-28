#include "ipc_server.h"
#include <QDebug>

IpcServer::IpcServer(const QString& pipeName, QObject* parent)
    : QObject(parent), m_pipeName(pipeName) {
}

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start() {
    stop();

    // Clean up any stale pipe instance
    QLocalServer::removeServer(m_pipeName);

    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);

    if (!m_server->listen(m_pipeName)) {
        qWarning() << "[IpcServer] Failed to listen on named pipe:" << m_pipeName << m_server->errorString();
        return false;
    }

    qInfo() << "[IpcServer] Listening on pipe:" << m_pipeName;
    return true;
}

void IpcServer::stop() {
    if (m_clientSocket) {
        m_clientSocket->disconnect();
        m_clientSocket->close();
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    QLocalServer::removeServer(m_pipeName);
}

void IpcServer::onNewConnection() {
    QLocalSocket* sock = m_server->nextPendingConnection();
    if (!sock) return;

    qInfo() << "[IpcServer] New client connection accepted.";

    // If we already have a client socket, close old one
    if (m_clientSocket) {
        m_clientSocket->disconnect();
        m_clientSocket->close();
        m_clientSocket->deleteLater();
    }

    m_clientSocket = sock;
    connect(m_clientSocket, &QLocalSocket::disconnected, this, &IpcServer::onSocketDisconnected);
    connect(m_clientSocket, &QLocalSocket::readyRead, this, &IpcServer::onReadyRead);
    emit sigActivity();
}

void IpcServer::onSocketDisconnected() {
    qInfo() << "[IpcServer] Client socket disconnected.";
    if (m_clientSocket) {
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
}

void IpcServer::onReadyRead() {
    if (!m_clientSocket) return;

    m_buffer.append(m_clientSocket->readAll());

    int newlineIdx;
    while ((newlineIdx = m_buffer.indexOf('\n')) != -1) {
        QByteArray lineBytes = m_buffer.left(newlineIdx);
        m_buffer.remove(0, newlineIdx + 1);

        QString line = QString::fromUtf8(lineBytes).trimmed();
        if (!line.isEmpty()) {
            processCommandLine(line);
        }
    }
}

void IpcServer::processCommandLine(const QString& line) {
    qDebug() << "[IpcServer] Recv command:" << line;
    emit sigActivity();

    QStringList tokens = line.split(' ', Qt::SkipEmptyParts);
    if (tokens.isEmpty()) return;

    const QString cmd = tokens[0].toUpper();

    if (cmd == "ATTACH") {
        if (tokens.size() >= 2) {
            bool ok = false;
            quint64 hwnd = tokens[1].toULongLong(&ok, 0); // supports 0x hex or decimal
            if (ok) {
                emit sigAttachHwnd(hwnd);
            }
        }
    } else if (cmd == "LOAD") {
        // Syntax: LOAD <generationId> <filePath...>
        if (tokens.size() >= 3) {
            bool ok = false;
            uint64_t genId = tokens[1].toULongLong(&ok, 10);
            int pathStart = line.indexOf(tokens[1]) + tokens[1].length();
            QString path = line.mid(pathStart).trimmed();
            if (ok && !path.isEmpty()) {
                emit sigLoadModel(genId, path);
            }
        }
    } else if (cmd == "RESIZE") {
        if (tokens.size() >= 3) {
            int w = tokens[1].toInt();
            int h = tokens[2].toInt();
            emit sigResize(w, h);
        }
    } else if (cmd == "CLEAR") {
        emit sigClearDoc();
        sendCleared();
    } else if (cmd == "PING") {
        sendPong();
    } else if (cmd == "QUIT") {
        emit sigQuit();
    }
}

void IpcServer::sendAttached(bool ok) {
    if (m_clientSocket && m_clientSocket->isValid()) {
        QString msg = ok ? "ATTACHED\n" : "ATTACH_FAILED\n";
        m_clientSocket->write(msg.toUtf8());
        m_clientSocket->flush();
    }
}

void IpcServer::sendLoaded(uint64_t genId, bool ok, const QString& errorMsg) {
    if (m_clientSocket && m_clientSocket->isValid()) {
        QString msg;
        if (ok) {
            msg = QString("LOADED %1 OK\n").arg(genId);
        } else {
            msg = QString("LOADED %1 ERROR %2\n").arg(genId).arg(errorMsg);
        }
        m_clientSocket->write(msg.toUtf8());
        m_clientSocket->flush();
    }
}

void IpcServer::sendCleared() {
    if (m_clientSocket && m_clientSocket->isValid()) {
        m_clientSocket->write("CLEARED\n");
        m_clientSocket->flush();
    }
}

void IpcServer::sendPong() {
    if (m_clientSocket && m_clientSocket->isValid()) {
        m_clientSocket->write("PONG\n");
        m_clientSocket->flush();
    }
}
