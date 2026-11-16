#include "googleauth.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QDebug>

#ifndef LOWACAL_GOOGLE_CLIENT_ID
#define LOWACAL_GOOGLE_CLIENT_ID ""
#endif
#ifndef LOWACAL_GOOGLE_CLIENT_SECRET
#define LOWACAL_GOOGLE_CLIENT_SECRET ""
#endif

namespace {

const char kCalendarScope[] = "https://www.googleapis.com/auth/calendar";

QString envOr(const char *name, const char *fallback)
{
    const QString fromEnv = qEnvironmentVariable(name);
    return fromEnv.isEmpty() ? QString::fromUtf8(fallback) : fromEnv;
}

// RFC 7636 "unreserved" characters only.
QString randomUrlSafe(int length)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString out;
    out.reserve(length);
    for (int i = 0; i < length; ++i)
        out.append(QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]));
    return out;
}

QByteArray formBody(const QList<QPair<QString, QString>> &fields)
{
    QByteArrayList parts;
    for (const auto &[key, value] : fields)
        parts.append(key.toUtf8() + '=' + QUrl::toPercentEncoding(value));
    return parts.join('&');
}

QNetworkRequest tokenRequest()
{
    QNetworkRequest req(GoogleConfig::tokenEndpoint());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    req.setTransferTimeout(20000);
    return req;
}

// The id_token came straight from Google's token endpoint over TLS, so its
// payload can be read without verifying the signature.
QString emailFromIdToken(const QString &idToken)
{
    const QStringList parts = idToken.split('.');
    if (parts.size() < 2)
        return QString();
    const QByteArray payload =
        QByteArray::fromBase64(parts.at(1).toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QJsonDocument::fromJson(payload).object().value("email").toString();
}

QByteArray browserPage(const QString &heading, const QString &text)
{
    const QString html = QStringLiteral(
                             "<!doctype html><html><head><meta charset=\"utf-8\"><title>Lowacal</title>"
                             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                             "<style>body{font-family:system-ui,sans-serif;background:#2a2e32;color:#fcfcfc;display:flex;"
                             "min-height:100vh;margin:0;align-items:center;justify-content:center;text-align:center}"
                             "main{max-width:26em;padding:2em}h1{font-size:1.4em}p{opacity:.8;line-height:1.5}</style>"
                             "</head><body><main><h1>%1</h1><p>%2</p></main></body></html>")
                             .arg(heading.toHtmlEscaped(), text.toHtmlEscaped());
    const QByteArray body = html.toUtf8();
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
        + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

} // namespace

QString GoogleConfig::clientId()
{
    return envOr("LOWACAL_GOOGLE_CLIENT_ID", LOWACAL_GOOGLE_CLIENT_ID);
}

QString GoogleConfig::clientSecret()
{
    return envOr("LOWACAL_GOOGLE_CLIENT_SECRET", LOWACAL_GOOGLE_CLIENT_SECRET);
}

bool GoogleConfig::isConfigured()
{
    return !clientId().isEmpty();
}

QUrl GoogleConfig::authEndpoint()
{
    return QUrl(envOr("LOWACAL_GOOGLE_AUTH_URL", "https://accounts.google.com/o/oauth2/v2/auth"));
}

QUrl GoogleConfig::tokenEndpoint()
{
    return QUrl(envOr("LOWACAL_GOOGLE_TOKEN_URL", "https://oauth2.googleapis.com/token"));
}

QString GoogleConfig::caldavBaseUrl()
{
    return envOr("LOWACAL_GOOGLE_CALDAV_URL", "https://apidata.googleusercontent.com/caldav/v2/");
}

GoogleSignIn::GoogleSignIn(QObject *parent) : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &GoogleSignIn::onNewConnection);
    m_timeout.setSingleShot(true);
    // People do wander off mid sign-in; don't keep a port open forever.
    m_timeout.setInterval(5 * 60 * 1000);
    connect(&m_timeout, &QTimer::timeout, this,
            [this] { finish(false, QString(), QString(), tr("Signing in to Google took too long. Try again.")); });
}

void GoogleSignIn::start()
{
    if (!GoogleConfig::isConfigured()) {
        finish(false, QString(), QString(), tr("This build of Lowacal isn't set up for Google accounts."));
        return;
    }
    // Loopback only, random free port - the redirect never leaves this machine.
    if (!m_server.listen(QHostAddress::LocalHost, 0)) {
        finish(false, QString(), QString(), tr("Couldn't start signing in to Google: %1").arg(m_server.errorString()));
        return;
    }
    m_redirectUri = QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    m_verifier = randomUrlSafe(64);
    m_state = randomUrlSafe(32);
    const QString challenge = QString::fromLatin1(
        QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    const QList<QPair<QString, QString>> params{
        {"client_id", GoogleConfig::clientId()},
        {"redirect_uri", m_redirectUri},
        {"response_type", "code"},
        // `email` only to label the account and to know whose calendars these are.
        {"scope", QStringLiteral("openid email ") + QLatin1String(kCalendarScope)},
        {"code_challenge", challenge},
        {"code_challenge_method", "S256"},
        {"state", m_state},
        {"access_type", "offline"},
        {"prompt", "consent"},
    };
    QUrl url = GoogleConfig::authEndpoint();
    url.setQuery(QString::fromLatin1(formBody(params)), QUrl::StrictMode);

    m_timeout.start();
    emit openBrowserRequested(url);
}

void GoogleSignIn::onNewConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            if (!socket->canReadLine())
                return;
            // "GET /?code=...&state=... HTTP/1.1"
            const QList<QByteArray> requestLine = socket->readLine().split(' ');
            const QUrl target = QUrl::fromEncoded(requestLine.value(1));
            const QUrlQuery query(target);
            const QString state = query.queryItemValue("state", QUrl::FullyDecoded);
            const QString code = query.queryItemValue("code", QUrl::FullyDecoded);
            const QString error = query.queryItemValue("error", QUrl::FullyDecoded);

            auto reply = [socket](const QByteArray &response) {
                socket->write(response);
                socket->disconnectFromHost();
            };

            // Anything that isn't our redirect (favicon requests, a stray
            // local port scan, a forged request without our state value)
            // is ignored and the sign-in keeps waiting.
            if (m_done || state != m_state || (code.isEmpty() && error.isEmpty())) {
                reply("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                return;
            }
            if (!error.isEmpty()) {
                reply(browserPage(tr("Sign-in cancelled"), tr("Nothing was connected. You can close this tab and return to Lowacal.")));
                finish(false, QString(), QString(),
                       error == QLatin1String("access_denied") ? tr("Signing in to Google was cancelled.")
                                                               : tr("Google reported an error: %1").arg(error));
                return;
            }
            reply(browserPage(tr("Almost done"), tr("You can close this tab and return to Lowacal.")));
            m_server.close(); // one code per attempt
            exchangeCode(code);
        });
    }
}

void GoogleSignIn::exchangeCode(const QString &code)
{
    QList<QPair<QString, QString>> fields{
        {"grant_type", "authorization_code"},
        {"code", code},
        {"client_id", GoogleConfig::clientId()},
        {"redirect_uri", m_redirectUri},
        {"code_verifier", m_verifier},
    };
    if (!GoogleConfig::clientSecret().isEmpty())
        fields.append({"client_secret", GoogleConfig::clientSecret()});

    QNetworkReply *reply = m_nam.post(tokenRequest(), formBody(fields));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
        if (status != 200) {
            qWarning() << "GoogleSignIn: token exchange failed:" << status << json.value("error").toString()
                       << json.value("error_description").toString();
            finish(false, QString(), QString(),
                   status == 0 ? tr("Couldn't reach Google. Check your internet connection.")
                               : tr("Google didn't accept the sign-in (%1).").arg(json.value("error").toString()));
            return;
        }
        const QString refreshToken = json.value("refresh_token").toString();
        const QString email = emailFromIdToken(json.value("id_token").toString());
        // Google's consent screen lets people untick individual permissions.
        const bool calendarGranted = json.value("scope").toString().split(' ').contains(QLatin1String(kCalendarScope));
        if (!calendarGranted) {
            finish(false, QString(), QString(),
                   tr("Access to your calendars wasn't granted. Sign in again and leave the calendar permission ticked."));
            return;
        }
        if (refreshToken.isEmpty() || email.isEmpty()) {
            qWarning() << "GoogleSignIn: response lacks refresh_token or email";
            finish(false, QString(), QString(), tr("Google's answer was incomplete. Try signing in again."));
            return;
        }
        finish(true, email, refreshToken, QString());
    });
}

void GoogleSignIn::finish(bool ok, const QString &email, const QString &refreshToken, const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    m_timeout.stop();
    m_server.close();
    emit finished(ok, email, refreshToken, error);
}

GoogleTokens::RefreshResult GoogleTokens::refreshAccessToken(const QString &refreshToken, QString *accessToken,
                                                             int *expiresInSeconds)
{
    QList<QPair<QString, QString>> fields{
        {"grant_type", "refresh_token"},
        {"refresh_token", refreshToken},
        {"client_id", GoogleConfig::clientId()},
    };
    if (!GoogleConfig::clientSecret().isEmpty())
        fields.append({"client_secret", GoogleConfig::clientSecret()});

    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.post(tokenRequest(), formBody(fields));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();

    if (status == 200 && !json.value("access_token").toString().isEmpty()) {
        *accessToken = json.value("access_token").toString();
        *expiresInSeconds = json.value("expires_in").toInt(3600);
        return RefreshResult::Ok;
    }
    qWarning() << "GoogleTokens: refresh failed:" << status << json.value("error").toString()
               << json.value("error_description").toString();
    // invalid_grant = revoked, expired (7 days while the Google Cloud
    // project is in "Testing"), or the password was changed.
    return json.value("error").toString() == QLatin1String("invalid_grant") ? RefreshResult::Revoked
                                                                            : RefreshResult::Failed;
}
