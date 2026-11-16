#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>

// Google Calendar speaks CalDAV like iCloud does, but refuses passwords:
// every request must carry an OAuth 2.0 access token. This file is the
// OAuth part; the CalDAV part is the same CalDavClient as everywhere else.
//
// What gets stored: only the refresh token, in the system keychain, in the
// same slot other accounts keep their password. Access tokens live for
// about an hour, are derived from it on demand and never written to disk.

namespace GoogleConfig {

// The OAuth client of *this build* of Lowacal, registered in Google Cloud
// (type "Desktop app"). Set at build time (-DLOWACAL_GOOGLE_CLIENT_ID=...,
// -DLOWACAL_GOOGLE_CLIENT_SECRET=...); the environment variables of the
// same names override it at run time, which is handy while setting the
// project up. For desktop apps Google does not treat the "secret" as
// confidential - it ships inside every copy of the app - which is why the
// flow below relies on PKCE instead.
QString clientId();
QString clientSecret();
bool isConfigured();

// Google's endpoints. Overridable through LOWACAL_GOOGLE_AUTH_URL /
// _TOKEN_URL / _CALDAV_URL so the tests can point the real code at a fake
// Google.
QUrl authEndpoint();
QUrl tokenEndpoint();
QString caldavBaseUrl();

} // namespace GoogleConfig

// One interactive sign-in: the authorization-code flow for installed apps,
// with PKCE and a loopback redirect, as Google's documentation prescribes.
//   1. listen on http://127.0.0.1:<random port>
//   2. ask for the user's browser to be opened on Google's consent page
//   3. Google redirects the browser back to the loopback port with a code
//   4. exchange the code (+ the PKCE verifier) for tokens
// The user's Google password is typed into their own browser and never
// passes through this app. GUI thread; one object per attempt.
class GoogleSignIn : public QObject {
    Q_OBJECT

public:
    explicit GoogleSignIn(QObject *parent = nullptr);

    void start();

signals:
    // Emitted instead of opening the browser directly, so the app can use
    // QDesktopServices (the portal, inside Flatpak) and tests can play the
    // browser themselves.
    void openBrowserRequested(const QUrl &url);
    // `error` is already a message for the user.
    void finished(bool ok, const QString &email, const QString &refreshToken, const QString &error);

private:
    void onNewConnection();
    void exchangeCode(const QString &code);
    void finish(bool ok, const QString &email, const QString &refreshToken, const QString &error);

    QTcpServer m_server;
    QNetworkAccessManager m_nam;
    QTimer m_timeout;
    QString m_verifier;
    QString m_state;
    QString m_redirectUri;
    bool m_done = false;
};

namespace GoogleTokens {

enum class RefreshResult {
    Ok,
    Revoked, // Google no longer honours the refresh token: the user must sign in again
    Failed, // network trouble or a server error: worth retrying later
};

// Trades the long-lived refresh token for a fresh access token. Blocks
// (local event loop) - call from SyncWorker's thread only, like CalDavClient.
RefreshResult refreshAccessToken(const QString &refreshToken, QString *accessToken, int *expiresInSeconds);

} // namespace GoogleTokens
