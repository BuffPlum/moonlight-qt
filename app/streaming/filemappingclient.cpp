#include "filemappingclient.h"

#include "backend/identitymanager.h"
#include "filemappingwebsocket.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>

namespace {
constexpr int kCancellationPollMs = 50;

QString compactJsonForLog(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString rpcReplyError(const QJsonObject& reply)
{
    if (reply.value(QStringLiteral("type")).toString() != QStringLiteral("error")) {
        return {};
    }

    QString message = reply.value(QStringLiteral("message")).toString();
    QString code = reply.value(QStringLiteral("code")).toString();
    if (message.isEmpty()) {
        message = QObject::tr("File mapping RPC returned an error");
    }
    if (!code.isEmpty()) {
        message += QObject::tr(" (%1)").arg(code);
    }
    return message;
}

QString clientUuid()
{
    return IdentityManager::get()->getUniqueId();
}
} // namespace

FileMappingClient::FileMappingClient(NvComputer* computer,
                                     const std::atomic_bool* cancelRequested,
                                     QObject* parent)
    : QObject(parent),
      m_Computer(computer),
      m_CancelRequested(cancelRequested)
{
}

FileMappingClient::~FileMappingClient()
{
    closeSession();
}

FileMappingClient::Capability FileMappingClient::fetchCapability(int timeoutMs)
{
    Capability capability;
    if (cancellationRequested()) {
        capability.error = tr("File mapping request was cancelled");
        return capability;
    }
    QUrl url;
    if (!buildCapabilityUrl(url)) {
        capability.error = tr("Missing host address, HTTPS port, certificate, or UUID");
        return capability;
    }

    QNetworkRequest request(url);
    request.setRawHeader("X-File-Mapping-Client-UUID", clientUuid().toUtf8());
    request.setSslConfiguration(sslConfiguration());

    QEventLoop loop;
    QTimer timer;
    QTimer cancellationTimer;
    bool timedOut = false;
    bool cancelled = false;
    timer.setSingleShot(true);
    QNetworkReply* reply = nam()->get(request);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();
        loop.quit();
    });
    if (m_CancelRequested != nullptr) {
        cancellationTimer.setInterval(kCancellationPollMs);
        connect(&cancellationTimer, &QTimer::timeout, &loop, [&]() {
            if (cancellationRequested()) {
                cancelled = true;
                reply->abort();
                loop.quit();
            }
        });
        cancellationTimer.start();
    }
    timer.start(timeoutMs);
    loop.exec();

    timer.stop();
    cancellationTimer.stop();
    if (cancelled) {
        capability.error = tr("File mapping request was cancelled");
        delete reply;
        return capability;
    }
    if (timedOut) {
        capability.error = tr("Timed out while fetching file mapping capability");
        delete reply;
        return capability;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseBody = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        capability.error = httpStatus > 0 ?
                    tr("Capability request failed with HTTP %1: %2").arg(httpStatus).arg(reply->errorString()) :
                    reply->errorString();
        if (!responseBody.isEmpty()) {
            capability.error += tr(" body=%1").arg(QString::fromUtf8(responseBody.left(512)));
        }
        delete reply;
        return capability;
    }

    delete reply;

    QJsonParseError parseError {};
    QJsonDocument doc = QJsonDocument::fromJson(responseBody, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        capability.error = tr("Capability response was not valid JSON: %1").arg(parseError.errorString());
        return capability;
    }

    QJsonObject body = doc.object();
    capability.ok = body.value(QStringLiteral("ok")).toBool(false);
    capability.enabled = body.value(QStringLiteral("enabled")).toBool(false);
    capability.listening = body.value(QStringLiteral("listening")).toBool(false);
    capability.port = static_cast<quint16>(body.value(QStringLiteral("port")).toInt(0));
    capability.sessionEndpoint = body.value(QStringLiteral("session_endpoint")).toString();
    capability.sessionUrl = body.value(QStringLiteral("session_url")).toString();
    capability.sessionToken = body.value(QStringLiteral("session_token")).toString();
    capability.clientUuid = body.value(QStringLiteral("client_uuid")).toString();
    capability.accessMode = body.value(QStringLiteral("access_mode")).toString(QStringLiteral("read_only"));
    capability.features = body.value(QStringLiteral("features")).toArray();
    capability.error = body.value(QStringLiteral("error")).toString();
    return capability;
}

bool FileMappingClient::Capability::supportsFullDiskAccess() const
{
    if (accessMode == QStringLiteral("full_disk")) {
        return true;
    }

    for (const QJsonValue& feature : features) {
        if (feature.toString() == QStringLiteral("buffplum_full_disk")) {
            return true;
        }
    }
    return false;
}

bool FileMappingClient::connectSession(const Capability& capability, int timeoutMs, QString* error)
{
    if (cancellationRequested()) {
        if (error != nullptr) {
            *error = tr("File mapping request was cancelled");
        }
        return false;
    }
    if (!capability.ok || !capability.enabled || !capability.listening) {
        if (error != nullptr) {
            *error = tr("File mapping capability is unavailable: ok=%1 enabled=%2 listening=%3 port=%4 error=%5")
                    .arg(capability.ok)
                    .arg(capability.enabled)
                    .arg(capability.listening)
                    .arg(capability.port)
                    .arg(capability.error);
        }
        return false;
    }

    QUrl sessionUrl;
    if (!buildSessionUrl(capability, sessionUrl)) {
        if (error != nullptr) {
            *error = tr("File mapping session URL is invalid");
        }
        return false;
    }

    closeSession();
    m_Socket = new QSslSocket(this);
    m_Socket->setSslConfiguration(sslConfiguration());
    connect(m_Socket, static_cast<void (QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors), this, [this](const QList<QSslError>& errors) {
        if (isPinnedCertificateError(errors)) {
            m_Socket->ignoreSslErrors(errors);
        }
    });

    m_Socket->connectToHostEncrypted(sessionUrl.host(), static_cast<quint16>(sessionUrl.port(443)));
    if (!waitForEncrypted(timeoutMs)) {
        if (error != nullptr) {
            *error = cancellationRequested()
                    ? tr("File mapping request was cancelled")
                    : m_Socket->errorString().isEmpty()
                            ? tr("Timed out while opening file mapping TLS connection")
                            : m_Socket->errorString();
        }
        closeSession();
        return false;
    }

    QByteArray nonce(16, Qt::Uninitialized);
    for (int i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    const QByteArray websocketKey = nonce.toBase64();

    QString target = sessionUrl.path().isEmpty() ? QStringLiteral("/") : sessionUrl.path();
    if (!sessionUrl.query().isEmpty()) {
        target += QStringLiteral("?") + sessionUrl.query(QUrl::FullyEncoded);
    }

    QString host = sessionUrl.host();
    if (host.contains(':') && !host.startsWith('[')) {
        host = QStringLiteral("[%1]").arg(host);
    }
    if (sessionUrl.port(443) != 443) {
        host += QStringLiteral(":%1").arg(sessionUrl.port());
    }

    QByteArray request;
    request += "GET " + target.toUtf8() + " HTTP/1.1\r\n";
    request += "Host: " + host.toUtf8() + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + websocketKey + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "\r\n";

    if (m_Socket->write(request) != request.size() || !waitForBytesWritten(timeoutMs)) {
        if (error != nullptr) {
            *error = cancellationRequested()
                    ? tr("File mapping request was cancelled")
                    : m_Socket->errorString().isEmpty()
                            ? tr("Failed to write file mapping WebSocket upgrade request")
                            : m_Socket->errorString();
        }
        closeSession();
        return false;
    }

    m_WsBuffer.clear();
    while (!m_WsBuffer.contains("\r\n\r\n")) {
        if (m_WsBuffer.size() > 64 * 1024) {
            if (error != nullptr) {
                *error = tr("File mapping WebSocket upgrade response is too large");
            }
            closeSession();
            return false;
        }
        if (!waitForReadyRead(timeoutMs)) {
            if (error != nullptr) {
                *error = cancellationRequested()
                        ? tr("File mapping request was cancelled")
                        : m_Socket->errorString().isEmpty()
                                ? tr("Timed out waiting for file mapping WebSocket upgrade")
                                : m_Socket->errorString();
            }
            closeSession();
            return false;
        }
        m_WsBuffer += m_Socket->readAll();
    }

    const int headerEnd = m_WsBuffer.indexOf("\r\n\r\n");
    const QByteArray headers = m_WsBuffer.left(headerEnd);
    m_WsBuffer.remove(0, headerEnd + 4);

    QList<QByteArray> headerLines = headers.split('\n');
    if (headerLines.isEmpty() ||
            !(headerLines.first().trimmed().startsWith("HTTP/1.1 101") ||
              headerLines.first().trimmed().startsWith("HTTP/1.0 101"))) {
        if (error != nullptr) {
            *error = tr("File mapping WebSocket upgrade was rejected: %1")
                    .arg(QString::fromUtf8(headerLines.value(0).trimmed()));
        }
        closeSession();
        return false;
    }

    QByteArray acceptHeader;
    for (int i = 1; i < headerLines.size(); ++i) {
        const QByteArray line = headerLines[i].trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        if (line.left(colon).trimmed().toLower() == "sec-websocket-accept") {
            acceptHeader = line.mid(colon + 1).trimmed();
            break;
        }
    }

    const QByteArray expectedAccept = QCryptographicHash::hash(
                                          websocketKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                          QCryptographicHash::Sha1)
                                          .toBase64();
    if (acceptHeader != expectedAccept) {
        if (error != nullptr) {
            *error = tr("File mapping WebSocket accept header is invalid");
        }
        closeSession();
        return false;
    }

    QJsonObject hello {
        { QStringLiteral("type"), QStringLiteral("hello") },
        { QStringLiteral("version"), 1 },
        { QStringLiteral("endpoint"), QStringLiteral("client") },
        { QStringLiteral("client_uuid"), capability.clientUuid.isEmpty() ? clientUuid() : capability.clientUuid },
        { QStringLiteral("mappings"), QJsonArray {} }
    };
    QString sendError;
    if (!sendAndWait(hello, m_LastHello, timeoutMs, &sendError)) {
        if (error != nullptr) {
            *error = sendError;
        }
        closeSession();
        return false;
    }
    sendError = rpcReplyError(m_LastHello);
    if (!sendError.isEmpty()) {
        if (error != nullptr) {
            *error = sendError;
        }
        closeSession();
        return false;
    }

    m_NextRequestId = 1;
    m_SessionConnected = true;
    return true;
}

FileMappingClient::RpcResult FileMappingClient::list(const QString& mappingId, const QString& path, int timeoutMs)
{
    return sendRpc({
        { QStringLiteral("type"), QStringLiteral("list") },
        { QStringLiteral("mapping"), mappingId },
        { QStringLiteral("path"), path }
    }, timeoutMs);
}

FileMappingClient::RpcResult FileMappingClient::stat(const QString& mappingId, const QString& path, int timeoutMs)
{
    return sendRpc({
        { QStringLiteral("type"), QStringLiteral("stat") },
        { QStringLiteral("mapping"), mappingId },
        { QStringLiteral("path"), path }
    }, timeoutMs);
}

FileMappingClient::RpcResult FileMappingClient::read(const QString& mappingId,
                                                     const QString& path,
                                                     quint64 offset,
                                                     quint32 length,
                                                     int timeoutMs)
{
    return sendRpc({
        { QStringLiteral("type"), QStringLiteral("read") },
        { QStringLiteral("mapping"), mappingId },
        { QStringLiteral("path"), path },
        { QStringLiteral("offset"), static_cast<double>(offset) },
        { QStringLiteral("length"), static_cast<int>(length) }
    }, timeoutMs);
}

FileMappingClient::RpcResult FileMappingClient::mkdir(const QString& mappingId,
                                                      const QString& path,
                                                      const QString& conflictPolicy,
                                                      int timeoutMs)
{
    return sendRpc({
        { QStringLiteral("type"), QStringLiteral("mkdir") },
        { QStringLiteral("mapping"), mappingId },
        { QStringLiteral("path"), path },
        { QStringLiteral("conflict_policy"), conflictPolicy }
    }, timeoutMs);
}

FileMappingClient::RpcResult FileMappingClient::rename(const QString& mappingId,
                                                       const QString& path,
                                                       const QString& destinationPath,
                                                       int timeoutMs)
{
    return sendRpc({
        { QStringLiteral("type"), QStringLiteral("rename") },
        { QStringLiteral("mapping"), mappingId },
        { QStringLiteral("path"), path },
        { QStringLiteral("destination_path"), destinationPath }
    }, timeoutMs);
}

FileMappingClient::RpcResult FileMappingClient::remove(const QString& mappingId,
                                                       const QString& path,
                                                       bool recursive,
                                                       int timeoutMs)
{
    return sendRpc({
        { QStringLiteral("type"), QStringLiteral("delete") },
        { QStringLiteral("mapping"), mappingId },
        { QStringLiteral("path"), path },
        { QStringLiteral("recursive"), recursive }
    }, timeoutMs);
}

FileMappingClient::RpcResult FileMappingClient::write(const QString& mappingId,
                                                      const QString& path,
                                                      const QString& uploadId,
                                                      quint64 offset,
                                                      quint64 totalSize,
                                                      const QByteArray& data,
                                                      bool begin,
                                                      bool complete,
                                                      const QString& conflictPolicy,
                                                      int timeoutMs)
{
    return sendRpc({
        { QStringLiteral("type"), QStringLiteral("write") },
        { QStringLiteral("mapping"), mappingId },
        { QStringLiteral("path"), path },
        { QStringLiteral("upload_id"), uploadId },
        { QStringLiteral("offset"), static_cast<double>(offset) },
        { QStringLiteral("total_size"), static_cast<double>(totalSize) },
        { QStringLiteral("begin"), begin },
        { QStringLiteral("complete"), complete },
        { QStringLiteral("conflict_policy"), conflictPolicy },
        { QStringLiteral("data"), QString::fromLatin1(data.toBase64()) }
    }, timeoutMs);
}

FileMappingClient::SmokeResult FileMappingClient::smokeRead(const QString& mappingId,
                                                            const QString& path,
                                                            quint64 offset,
                                                            quint32 length,
                                                            int timeoutMs)
{
    SmokeResult result;
    Capability capability = fetchCapability(timeoutMs);
    if (!connectSession(capability, timeoutMs, &result.error)) {
        return result;
    }

    result.hello = m_LastHello;
    RpcResult listResult = list(mappingId, QString(), timeoutMs);
    result.list = listResult.reply;
    if (!listResult.ok) {
        result.error = listResult.error;
        closeSession();
        return result;
    }

    RpcResult readResult = read(mappingId, path, offset, length, timeoutMs);
    result.read = readResult.reply;
    if (!readResult.ok) {
        result.error = readResult.error;
        closeSession();
        return result;
    }

    result.ok = m_LastHello.value(QStringLiteral("type")).toString() == QStringLiteral("hello") &&
                result.list.value(QStringLiteral("type")).toString() == QStringLiteral("result") &&
                result.read.value(QStringLiteral("type")).toString() == QStringLiteral("result");
    if (!result.ok) {
        result.error = tr("Unexpected file mapping smoke response: hello=%1 list=%2 read=%3")
                       .arg(compactJsonForLog(result.hello),
                            compactJsonForLog(result.list),
                            compactJsonForLog(result.read));
    }
    closeSession();
    return result;
}

bool FileMappingClient::sendAndWait(const QJsonObject& message, QJsonObject& out, int timeoutMs, QString* error)
{
    if (cancellationRequested()) {
        if (error != nullptr) {
            *error = tr("File mapping request was cancelled");
        }
        return false;
    }
    if (m_Socket == nullptr) {
        if (error != nullptr) {
            *error = tr("File mapping WebSocket session is not connected");
        }
        return false;
    }
    if (!FileMappingWebSocket::writeText(*m_Socket, QJsonDocument(message).toJson(QJsonDocument::Compact))) {
        if (error != nullptr) {
            *error = m_Socket->errorString().isEmpty() ? tr("Failed to write file mapping WebSocket message") : m_Socket->errorString();
        }
        return false;
    }

    QString readError = FileMappingWebSocket::readJsonText(
            *m_Socket, m_WsBuffer, out, timeoutMs, m_CancelRequested);
    if (!readError.isEmpty()) {
        if (error != nullptr) {
            *error = readError;
        }
        return false;
    }
    return true;
}

FileMappingClient::RpcResult FileMappingClient::sendRpc(QJsonObject message, int timeoutMs)
{
    RpcResult result;
    if (!m_SessionConnected) {
        result.error = tr("File mapping WebSocket session is not connected");
        return result;
    }

    message.insert(QStringLiteral("id"), static_cast<double>(m_NextRequestId++));
    if (!sendAndWait(message, result.reply, timeoutMs, &result.error)) {
        return result;
    }

    result.error = rpcReplyError(result.reply);
    result.ok = result.error.isEmpty() &&
                result.reply.value(QStringLiteral("type")).toString() == QStringLiteral("result") &&
                result.reply.value(QStringLiteral("ok")).toBool(true);
    if (!result.ok && result.error.isEmpty()) {
        result.error = tr("Unexpected file mapping RPC response: %1").arg(compactJsonForLog(result.reply));
    }
    return result;
}

void FileMappingClient::closeSession()
{
    m_SessionConnected = false;
    m_WsBuffer.clear();
    m_LastHello = {};
    m_NextRequestId = 1;
    if (m_Socket != nullptr) {
        m_Socket->close();
        delete m_Socket;
        m_Socket = nullptr;
    }
}

bool FileMappingClient::cancellationRequested() const
{
    return m_CancelRequested != nullptr &&
           m_CancelRequested->load(std::memory_order_relaxed);
}

bool FileMappingClient::waitForEncrypted(int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (cancellationRequested()) {
            return false;
        }
        if (m_Socket->isEncrypted()) {
            return true;
        }
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        const int waitMs = m_CancelRequested == nullptr
                ? remaining
                : qMin(remaining, kCancellationPollMs);
        if (m_Socket->waitForEncrypted(waitMs)) {
            return true;
        }
        if (m_Socket->state() == QAbstractSocket::UnconnectedState) {
            return false;
        }
    }
    return false;
}

bool FileMappingClient::waitForBytesWritten(int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (cancellationRequested()) {
            return false;
        }
        if (m_Socket->bytesToWrite() == 0) {
            return true;
        }
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        const int waitMs = m_CancelRequested == nullptr
                ? remaining
                : qMin(remaining, kCancellationPollMs);
        if (m_Socket->waitForBytesWritten(waitMs)) {
            return true;
        }
        if (m_Socket->state() == QAbstractSocket::UnconnectedState) {
            return false;
        }
    }
    return false;
}

bool FileMappingClient::waitForReadyRead(int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (cancellationRequested()) {
            return false;
        }
        if (m_Socket->bytesAvailable() > 0) {
            return true;
        }
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        const int waitMs = m_CancelRequested == nullptr
                ? remaining
                : qMin(remaining, kCancellationPollMs);
        if (m_Socket->waitForReadyRead(waitMs)) {
            return true;
        }
        if (m_Socket->state() == QAbstractSocket::UnconnectedState) {
            return false;
        }
    }
    return false;
}

bool FileMappingClient::buildCapabilityUrl(QUrl& outUrl) const
{
    if (m_Computer == nullptr ||
            m_Computer->activeAddress.isNull() ||
            m_Computer->activeHttpsPort == 0 ||
            m_Computer->serverCert.isNull() ||
            clientUuid().isEmpty()) {
        return false;
    }

    QString host = m_Computer->activeAddress.address();
    if (host.contains(':')) {
        host = QStringLiteral("[%1]").arg(host);
    }

    outUrl = QUrl(QStringLiteral("https://%1:%2/api/v1/file-mapping/capability")
                  .arg(host)
                  .arg(m_Computer->activeHttpsPort));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_uuid"), clientUuid());
    outUrl.setQuery(query);
    return outUrl.isValid();
}

bool FileMappingClient::buildSessionUrl(const Capability& capability, QUrl& outUrl) const
{
    if (!capability.sessionUrl.isEmpty()) {
        outUrl = QUrl(capability.sessionUrl);
    }
    else {
        if (m_Computer == nullptr || m_Computer->activeAddress.isNull() || capability.port == 0) {
            return false;
        }

        QString endpoint = capability.sessionEndpoint.isEmpty()
                ? QStringLiteral("/api/v1/file-mapping/session")
                : capability.sessionEndpoint;
        if (!endpoint.startsWith('/')) {
            endpoint.prepend('/');
        }

        outUrl = QUrl();
        outUrl.setScheme(QStringLiteral("wss"));
        outUrl.setHost(m_Computer->activeAddress.address());
        outUrl.setPort(capability.port);
        outUrl.setPath(endpoint);
    }

    if (!outUrl.isValid() || outUrl.scheme() != QStringLiteral("wss") || outUrl.host().isEmpty()) {
        return false;
    }

    if (!capability.sessionToken.isEmpty()) {
        QUrlQuery query(outUrl);
        if (!query.hasQueryItem(QStringLiteral("token"))) {
            query.addQueryItem(QStringLiteral("token"), capability.sessionToken);
            outUrl.setQuery(query);
        }
    }

    return true;
}

QNetworkAccessManager* FileMappingClient::nam()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
        connect(m_Nam, &QNetworkAccessManager::sslErrors, this,
                [this](QNetworkReply* reply, const QList<QSslError>& errors) {
                    if (isPinnedCertificateError(errors)) {
                        reply->ignoreSslErrors(errors);
                    }
                });
    }
    return m_Nam;
}

QSslConfiguration FileMappingClient::sslConfiguration() const
{
    QSslConfiguration config = IdentityManager::get()->getSslConfig();
    return config;
}

bool FileMappingClient::isPinnedCertificateError(const QList<QSslError>& errors) const
{
    if (m_Computer == nullptr || m_Computer->serverCert.isNull()) {
        return false;
    }
    for (const QSslError& error : errors) {
        if (error.certificate() != m_Computer->serverCert) {
            return false;
        }
    }
    return !errors.isEmpty();
}
