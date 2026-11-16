#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

// A small CalDAV client covering exactly what this app needs: discover
// where a user's calendars live (RFC 5397 + RFC 4791), list them, pull
// events in a time window, and upload new ones. Speaks plain WebDAV/
// CalDAV over HTTP with Basic auth, which is what iCloud expects (with
// an app-specific password) and what most self-hosted servers
// (Nextcloud, Baikal, Radicale...) accept too.
//
// Google Calendar is the outlier: its CalDAV endpoint requires a Google
// Cloud project + OAuth rather than a username/password - a separate
// flow this client doesn't implement. Point it at
// https://caldav.icloud.com or a self-hosted server and it should work
// as-is.
//
// THREADING: every method here blocks (via a local QEventLoop) until its
// network request completes. That's intentional and only safe because
// this class is only ever used from SyncWorker's dedicated thread, never
// the GUI thread - see syncworker.h. It reads like synchronous code
// (easier to follow for a multi-step discovery chain than nested
// callbacks would be) without actually blocking the UI, since it's not
// running there.
class CalDavClient : public QObject {
    Q_OBJECT

public:
    struct DiscoveredCalendar {
        QString href;
        QString displayName;
        QString color;
        QString ctag;
    };

    explicit CalDavClient(QString username, QString password, QObject *parent = nullptr);

    // Switches this client from Basic auth (username + password) to an
    // OAuth 2.0 access token ("Authorization: Bearer ..."), which is the
    // only thing Google's CalDAV endpoint accepts.
    void setBearerToken(const QString &accessToken) { m_bearerToken = accessToken; }

    // RFC 5397: ask the server who the current user's principal is.
    QString discoverPrincipal(const QString &baseUrl, bool *ok);

    // RFC 4791: ask the principal where its calendars live.
    QString discoverCalendarHome(const QString &baseUrl, const QString &principalHref, bool *ok);

    // Lists the calendar collections inside the home set, each with its
    // ctag (a cheap "has anything in this calendar changed?" checksum).
    QList<DiscoveredCalendar> listCalendars(const QString &baseUrl, const QString &homeHref, bool *ok);

    // Fetches every VEVENT resource in calendarHref overlapping
    // [fromUtc, toUtc) (iCalendar UTC form, e.g. "20260101T000000Z").
    // Returns (href, icsBody) pairs - href is what actually identifies a
    // resource uniquely on the server; the ICS body's own UID field is
    // not reliably unique (confirmed against real iCloud data, see
    // README), so callers should key storage off href, not UID.
    QList<QPair<QString, QString>> fetchEvents(const QString &baseUrl, const QString &calendarHref,
                                                const QString &fromUtc, const QString &toUtc, bool *ok);

    // Uploads a brand new event as "{calendarHref}{uid}.ics". Fails
    // loudly (rather than overwriting) if a resource already exists at
    // that URL. Returns the href the event was stored at (relative to
    // baseUrl, same form as everywhere else in this class) on success,
    // or an empty string on failure - the caller needs this href to
    // store the event under the right key (see README on why href, not
    // UID, is the identity key).
    QString putEvent(const QString &baseUrl, const QString &calendarHref, const QString &uid,
                      const QString &icsBody);

    // Overwrites the event resource at `eventHref` (unconditional PUT -
    // no If-Match ETag check yet, so a concurrent edit from another
    // device between our last sync and this write would be silently
    // clobbered; acceptable for v1, noted in the README).
    bool updateEvent(const QString &baseUrl, const QString &eventHref, const QString &icsBody);

    // Deletes the event resource at `eventHref`.
    bool deleteEvent(const QString &baseUrl, const QString &eventHref);

    QString lastError() const { return m_lastError; }
    // HTTP status of the last response (0 = no response at all: DNS,
    // refused connection, TLS failure, timeout). Lets callers turn
    // failures into messages a user can act on.
    int lastStatus() const { return m_lastStatus; }

    // Finds a working CalDAV endpoint from whatever the user typed (bare
    // host, web UI URL, ...) by trying it plus the standard well-known
    // locations. Returns the endpoint URL that answered (after redirects),
    // and its current-user-principal href via principalHref.
    QString discoverEndpoint(const QString &userUrl, QString *principalHref, bool *ok);

    // Public specifically so tests/logic_test.cpp can verify them against
    // real captured server responses - pure XML-in, data-out functions,
    // no network/state involved, safe to expose.
    static QList<DiscoveredCalendar> parseCalendarList(const QByteArray &xml);
    static QList<QPair<QString, QString>> extractCalendarData(const QByteArray &xml);

private:
    QByteArray webdavRequest(const QString &method, const QString &url, const QString &depth, const QByteArray &body,
                              bool *ok, const QString &debugLabel = QString());

    static QString joinUrl(const QString &base, const QString &href);
    static QString extractFirstHref(const QByteArray &xml, const QString &within);

    QNetworkAccessManager m_nam;
    QString m_username;
    QString m_password;
    QString m_bearerToken;
    QByteArray authHeader() const;
    QString m_lastError;
    QString m_lastFinalUrl;
    int m_lastStatus = 0;
};
