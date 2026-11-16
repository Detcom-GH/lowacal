#include "syncworker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <QUuid>
#include <QDebug>
#include <algorithm>

#include "caldavclient.h"
#include "googleauth.h"
#include "icalparser.h"

namespace {
constexpr int kPollIntervalMs = 10 * 60 * 1000; // 10 хв - див. README про те, чому не push
constexpr int kNotifyCheckIntervalMs = 60 * 1000;
constexpr int kWindowPastDays = 30;
constexpr int kWindowFutureDays = 180;
} // namespace

SyncWorker::SyncWorker(QObject *parent) : QObject(parent) {}

void SyncWorker::start()
{
    m_db.open(Database::defaultPath());
    // m_accounts starts empty on purpose: CalendarBridge reads the
    // accounts + their keychain passwords on the GUI thread and pushes
    // them via setAccounts(), which triggers the first real sync.

    // Persisted via QSettings on the GUI side (SettingsDialog) - read the
    // saved values here so reminders survive a restart even before the
    // GUI ever touches them this session.
    QSettings settings;
    m_alert1Minutes = settings.value("reminders/alert1Minutes", 10).toInt();
    m_alert2Minutes = settings.value("reminders/alert2Minutes", -1).toInt();

    m_syncTimer = new QTimer(this);
    connect(m_syncTimer, &QTimer::timeout, this, &SyncWorker::runFullSync);
    m_syncTimer->start(kPollIntervalMs);

    m_notifyTimer = new QTimer(this);
    connect(m_notifyTimer, &QTimer::timeout, this, &SyncWorker::checkNotifications);
    m_notifyTimer->start(kNotifyCheckIntervalMs);

}

void SyncWorker::requestSyncNow()
{
    runFullSync();
}

void SyncWorker::shutdown()
{
    // Runs on the worker thread (invoked via a queued connection from
    // main.cpp) - the only thread that may stop these timers.
    if (m_syncTimer)
        m_syncTimer->stop();
    if (m_notifyTimer)
        m_notifyTimer->stop();
}

void SyncWorker::requestCreateEvent(const QString &calendarId, const QString &summary, const QString &startsIso,
                                     const QString &endsIso, const QString &location, const QString &description)
{
    doCreateEvent(calendarId, summary, startsIso, endsIso, location, description);
}

const AccountConfig *SyncWorker::accountForCalendar(const QString &calendarId, QString *calendarHref) const
{
    const int sep = calendarId.indexOf(':');
    if (sep < 0)
        return nullptr;
    const QString accountId = calendarId.left(sep);
    if (calendarHref)
        *calendarHref = calendarId.mid(sep + 1);
    for (const AccountConfig &a : m_accounts) {
        if (a.id == accountId)
            return &a;
    }
    return nullptr;
}

void SyncWorker::requestDeleteEvent(const QString &calendarId, const QString &href)
{
    const AccountConfig *account = accountForCalendar(calendarId);
    if (!account) {
        emit syncStatusChanged(false, "no configured account matches this calendar");
        return;
    }

    QString authError;
    const std::unique_ptr<CalDavClient> authedClient = clientFor(*account, &authError);
    if (!authedClient) {
        emit syncStatusChanged(false, authError);
        return;
    }
    CalDavClient &client = *authedClient;
    if (!client.deleteEvent(account->baseUrl, href)) {
        emit syncStatusChanged(false, QStringLiteral("could not delete event: %1").arg(client.lastError()));
        return;
    }

    m_db.deleteEvent(href, calendarId);
    qInfo() << "sync: deleted event" << href;
    emit syncStatusChanged(false, QString());
}

void SyncWorker::requestUpdateEvent(const QString &calendarId, const QString &href, const QString &uid,
                                     const QString &summary, const QString &startsIso, const QString &endsIso,
                                     const QString &location, const QString &description)
{
    const AccountConfig *account = accountForCalendar(calendarId);
    if (!account) {
        emit syncStatusChanged(false, "no configured account matches this calendar");
        return;
    }

    const QDateTime starts = QDateTime::fromString(startsIso, Qt::ISODate).toUTC();
    const QDateTime ends = QDateTime::fromString(endsIso, Qt::ISODate).toUTC();
    if (!starts.isValid() || !ends.isValid()) {
        emit syncStatusChanged(false, "invalid event start/end time");
        return;
    }

    // Keep the existing UID - it's the same logical event, just edited.
    const QString ics = IcalParser::buildVEventIcs(uid, summary, starts, ends, location, description);

    QString authError;
    const std::unique_ptr<CalDavClient> authedClient = clientFor(*account, &authError);
    if (!authedClient) {
        emit syncStatusChanged(false, authError);
        return;
    }
    CalDavClient &client = *authedClient;
    if (!client.updateEvent(account->baseUrl, href, ics)) {
        emit syncStatusChanged(false, QStringLiteral("could not update event: %1").arg(client.lastError()));
        return;
    }

    EventRow row;
    row.uid = uid;
    row.href = href;
    row.calendarId = calendarId;
    row.summary = summary;
    row.description = description;
    row.location = location;
    row.startsAtUtc = starts.toString(Qt::ISODate);
    row.endsAtUtc = ends.toString(Qt::ISODate);
    row.allDay = false;
    m_db.upsertEvent(row);

    qInfo() << "sync: updated event" << summary;
    emit syncStatusChanged(false, QString());
}

void SyncWorker::setReminderOffsets(int alert1Minutes, int alert2Minutes)
{
    m_alert1Minutes = alert1Minutes;
    m_alert2Minutes = alert2Minutes;
}

void SyncWorker::setAccounts(const QList<AccountConfig> &accounts)
{
    // The list was assembled on the GUI thread, where reading passwords
    // from the keychain can take a while (KWallet may be asking the user
    // to unlock it). An account removed in the meantime must stay removed:
    // the accounts table is the truth, this list only adds the passwords.
    QSet<QString> existing;
    for (const AccountConfig &a : m_db.listAccounts())
        existing.insert(a.id);
    m_accounts.clear();
    for (const AccountConfig &a : accounts) {
        if (existing.contains(a.id))
            m_accounts.append(a);
    }
    runFullSync();
}

namespace {
// Turns a CalDavClient failure into something a user can act on, rather
// than "CalDAV server returned 401 for https://...".
QString humanAccountError(const QString &kind, int status)
{
    if ((status == 401 || status == 403) && kind == QLatin1String("google"))
        return QCoreApplication::translate("SyncWorker", "Google refused access to your calendars. Try signing in again.");
    if (status == 401 || status == 403) {
        if (kind == QLatin1String("icloud"))
            return QCoreApplication::translate("SyncWorker", "Wrong Apple ID or password. iCloud needs an "
                                               "app-specific password, not your main Apple ID password.");
        return QCoreApplication::translate("SyncWorker", "Wrong username or password.");
    }
    if (status == 0)
        return QCoreApplication::translate("SyncWorker",
                                           "Couldn't connect to the server. Check the address and your internet connection.");
    return QCoreApplication::translate("SyncWorker", "No CalDAV server found at this address (response %1). Check the "
                                       "address: for Nextcloud it's usually just your cloud server's address.")
        .arg(status);
}
} // namespace

void SyncWorker::verifyAccount(const QString &kind, const QString &label, const QString &url,
                               const QString &username, const QString &password)
{
    const bool google = kind == QLatin1String("google");
    const QString typedUrl = google ? GoogleConfig::caldavBaseUrl()
        : kind == QLatin1String("icloud") ? QStringLiteral("https://caldav.icloud.com") : url;
    if (typedUrl.trimmed().isEmpty()) {
        emit accountVerifyFailed(tr("Enter the server address."));
        return;
    }

    // For Google, `username` is the account's email and `password` is the
    // OAuth refresh token that GoogleSignIn just obtained.
    AccountConfig probe;
    probe.kind = kind;
    probe.username = username.trimmed();
    probe.password = password;
    QString authError;
    const std::unique_ptr<CalDavClient> authedClient = clientFor(probe, &authError);
    if (!authedClient) {
        emit accountVerifyFailed(authError);
        return;
    }
    CalDavClient &client = *authedClient;

    bool ok = false;
    QString principal;
    QString endpoint;
    if (google) {
        // Google's endpoint is fixed - no guessing of well-known paths.
        endpoint = typedUrl;
        probe.baseUrl = endpoint;
        principal = principalFor(client, probe, &ok);
    } else {
        endpoint = client.discoverEndpoint(typedUrl, &principal, &ok);
    }
    if (!ok) {
        emit accountVerifyFailed(humanAccountError(kind, client.lastStatus()));
        return;
    }

    const QString home = client.discoverCalendarHome(endpoint, principal, &ok);
    if (!ok) {
        emit accountVerifyFailed(humanAccountError(kind, client.lastStatus()));
        return;
    }
    const QList<CalDavClient::DiscoveredCalendar> calendars = client.listCalendars(endpoint, home, &ok);
    if (!ok) {
        emit accountVerifyFailed(humanAccountError(kind, client.lastStatus()));
        return;
    }

    AccountConfig account;
    // 8 hex chars of a UUID: unique enough for a handful of local
    // accounts, and contains no ':' (calendar ids are "<accountId>:<href>").
    account.id = QStringLiteral("acc-") + QUuid::createUuid().toString(QUuid::Id128).left(8);
    account.kind = kind;
    account.baseUrl = endpoint;
    account.username = username.trimmed();
    account.password = password;
    account.label = label.trimmed();
    if (account.label.isEmpty())
        account.label = kind == QLatin1String("icloud") ? QStringLiteral("iCloud")
            : google ? QStringLiteral("Google") : QUrl(endpoint).host();

    emit accountVerified(account, calendars.size());
}

void SyncWorker::commitAccount(const AccountConfig &account)
{
    m_db.upsertAccount(account.id, account.kind, account.label, account.baseUrl, account.username);
    m_accounts.append(account);
    emit accountsChanged();

    emit syncStatusChanged(true, QString());
    const QDateTime from = QDateTime::currentDateTimeUtc().addDays(-kWindowPastDays);
    const QDateTime to = QDateTime::currentDateTimeUtc().addDays(kWindowFutureDays);
    QString accountError;
    const bool ok =
        syncAccount(account, from.toString("yyyyMMddTHHmmss'Z'"), to.toString("yyyyMMddTHHmmss'Z'"), &accountError);
    if (!ok && accountError.isEmpty())
        accountError = tr("%1: sync failed").arg(account.label);
    emit syncStatusChanged(false, ok ? QString() : accountError);
}

void SyncWorker::removeAccount(const QString &accountId)
{
    m_db.deleteAccount(accountId);
    m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                    [&](const AccountConfig &a) { return a.id == accountId; }),
                     m_accounts.end());
    emit accountsChanged();
    emit syncStatusChanged(false, QString()); // also nudges the UI to re-read calendars/events
}

QString SyncWorker::principalFor(CalDavClient &client, const AccountConfig &account, bool *ok)
{
    const QString principal = client.discoverPrincipal(account.baseUrl, ok);
    if (*ok || account.kind != QLatin1String("google"))
        return principal;
    if (client.lastStatus() == 401 || client.lastStatus() == 403)
        return QString();
    // Google answered but wouldn't name the principal: use the address
    // Google documents for it, <base>/<email>/user.
    *ok = true;
    return QUrl(account.baseUrl).path() + QString::fromLatin1(QUrl::toPercentEncoding(account.username))
        + QStringLiteral("/user");
}

std::unique_ptr<CalDavClient> SyncWorker::clientFor(const AccountConfig &account, QString *error)
{
    auto client = std::make_unique<CalDavClient>(account.username, account.password);
    if (account.kind != QLatin1String("google"))
        return client;

    const QString &refreshToken = account.password;
    AccessToken cached = m_googleTokens.value(refreshToken);
    // A minute of margin, so a token never expires halfway through a sync.
    if (cached.token.isEmpty() || cached.expires < QDateTime::currentDateTimeUtc().addSecs(60)) {
        int expiresIn = 0;
        switch (GoogleTokens::refreshAccessToken(refreshToken, &cached.token, &expiresIn)) {
        case GoogleTokens::RefreshResult::Ok:
            cached.expires = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
            m_googleTokens.insert(refreshToken, cached);
            break;
        case GoogleTokens::RefreshResult::Revoked:
            m_googleTokens.remove(refreshToken);
            if (error)
                *error = tr("Google sign-in for %1 is no longer valid. Remove the account and add it again.")
                             .arg(account.username);
            return nullptr;
        case GoogleTokens::RefreshResult::Failed:
            if (error)
                *error = tr("Couldn't reach Google. Check your internet connection.");
            return nullptr;
        }
    }
    client->setBearerToken(cached.token);
    return client;
}

void SyncWorker::runFullSync()
{
    if (m_accounts.isEmpty()) {
        emit syncStatusChanged(false, QString());
        return;
    }

    emit syncStatusChanged(true, QString());

    const QDateTime from = QDateTime::currentDateTimeUtc().addDays(-kWindowPastDays);
    const QDateTime to = QDateTime::currentDateTimeUtc().addDays(kWindowFutureDays);
    const QString fromIcal = from.toString("yyyyMMddTHHmmss'Z'");
    const QString toIcal = to.toString("yyyyMMddTHHmmss'Z'");

    QString lastError;
    for (const AccountConfig &account : m_accounts) {
        QString accountError;
        if (!syncAccount(account, fromIcal, toIcal, &accountError))
            lastError = accountError.isEmpty() ? tr("%1: sync failed").arg(account.label) : accountError;
    }

    emit syncStatusChanged(false, lastError);
}

bool SyncWorker::syncAccount(const AccountConfig &account, const QString &fromIcal, const QString &toIcal,
                             QString *error)
{
    const bool google = account.kind == QLatin1String("google");
    std::unique_ptr<CalDavClient> authedClient;
    QString principal;
    bool ok = false;
    for (int attempt = 0;; ++attempt) {
        QString authError;
        authedClient = clientFor(account, &authError);
        if (!authedClient) {
            if (error)
                *error = authError;
            return false;
        }
        principal = principalFor(*authedClient, account, &ok);
        if (ok)
            break;
        // Google said the access token is no good although we believed it
        // was still valid (access revoked, clock skew...): forget it and
        // try once more with a freshly issued one. If the refresh token is
        // dead too, clientFor() reports that in plain words.
        if (attempt == 0 && google && authedClient->lastStatus() == 401) {
            m_googleTokens.remove(account.password);
            continue;
        }
        qWarning() << "sync:" << account.label << "principal discovery failed:" << authedClient->lastError();
        return false;
    }
    CalDavClient &client = *authedClient;

    const QString home = client.discoverCalendarHome(account.baseUrl, principal, &ok);
    if (!ok) {
        qWarning() << "sync:" << account.label << "home-set discovery failed:" << client.lastError();
        return false;
    }

    const QList<CalDavClient::DiscoveredCalendar> calendars = client.listCalendars(account.baseUrl, home, &ok);
    if (!ok) {
        qWarning() << "sync:" << account.label << "calendar listing failed:" << client.lastError();
        return false;
    }

    bool allOk = true;
    for (const CalDavClient::DiscoveredCalendar &cal : calendars) {
        const QString calendarId = account.id + ":" + cal.href;

        const bool unchanged = !cal.ctag.isEmpty() && m_db.calendarCtag(calendarId) == cal.ctag;
        m_db.upsertCalendar(calendarId, account.id, cal.href, cal.displayName, cal.color, cal.ctag);

        if (unchanged) {
            qDebug() << "sync:" << cal.displayName << "ctag unchanged, skipping re-fetch";
            continue;
        }

        bool fetchOk = false;
        const QList<QPair<QString, QString>> icsBlobs =
            client.fetchEvents(account.baseUrl, cal.href, fromIcal, toIcal, &fetchOk);
        if (!fetchOk) {
            qWarning() << "sync:" << cal.displayName << "event fetch failed:" << client.lastError();
            allOk = false;
            continue;
        }

        QList<EventRow> events;
        QSet<QString> uniqueUids;
        for (const auto &[eventHref, blob] : icsBlobs) {
            for (EventRow &row : IcalParser::parseVEvents(blob, calendarId, eventHref)) {
                uniqueUids.insert(row.uid);
                events.append(row);
            }
        }
        if (uniqueUids.size() != events.size()) {
            // Purely informational now - href (the server-guaranteed
            // unique resource identifier) is the real storage key, so a
            // UID collision no longer overwrites/loses data the way it
            // did before. Real iCloud data has shown this can legitimately
            // happen (distinct events created via some path that didn't
            // regenerate the UID) - see README.
            qInfo() << "sync:" << cal.displayName << "-" << events.size() << "events across" << uniqueUids.size()
                    << "unique UIDs (some events share a UID; each still stored separately, keyed by href)";
        }

        m_db.replaceEventsForCalendar(calendarId, events);
        qInfo() << "sync:" << cal.displayName << "-" << events.size() << "events";
    }

    return allOk;
}

void SyncWorker::doCreateEvent(const QString &calendarId, const QString &summary, const QString &startsIso,
                                const QString &endsIso, const QString &location, const QString &description)
{
    QString calendarHref;
    const AccountConfig *account = accountForCalendar(calendarId, &calendarHref);
    if (!account) {
        emit syncStatusChanged(false, "no configured account matches this calendar");
        return;
    }

    const QDateTime starts = QDateTime::fromString(startsIso, Qt::ISODate).toUTC();
    const QDateTime ends = QDateTime::fromString(endsIso, Qt::ISODate).toUTC();
    if (!starts.isValid() || !ends.isValid()) {
        emit syncStatusChanged(false, "invalid event start/end time");
        return;
    }

    const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString ics = IcalParser::buildVEventIcs(uid, summary, starts, ends, location, description);

    QString authError;
    const std::unique_ptr<CalDavClient> authedClient = clientFor(*account, &authError);
    if (!authedClient) {
        emit syncStatusChanged(false, authError);
        return;
    }
    CalDavClient &client = *authedClient;
    const QString eventHref = client.putEvent(account->baseUrl, calendarHref, uid, ics);
    if (eventHref.isEmpty()) {
        emit syncStatusChanged(false, QStringLiteral("could not create event: %1").arg(client.lastError()));
        return;
    }

    EventRow row;
    row.uid = uid;
    row.href = eventHref;
    row.calendarId = calendarId;
    row.summary = summary;
    row.description = description;
    row.location = location;
    row.startsAtUtc = starts.toString(Qt::ISODate);
    row.endsAtUtc = ends.toString(Qt::ISODate);
    row.allDay = false;
    m_db.upsertEvent(row);

    qInfo() << "sync: created new event" << summary;
    emit syncStatusChanged(false, QString());
}

void SyncWorker::checkNotifications()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // Two independent alert slots, each with its own lookahead window and
    // its own "already fired?" bookkeeping (the alert_slot column).
    // Processed shortest-window-first, and each event fires AT MOST ONE
    // popup per check: when an event is inside both windows at once
    // (e.g. created 10 minutes before it starts, with alerts at 10 and
    // 60 min), the nearer alert wins and the longer one is marked done
    // silently - otherwise both popped simultaneously (observed live).
    struct Alert { int slot; int minutes; };
    QList<Alert> alerts;
    if (m_alert1Minutes >= 0)
        alerts.append({1, m_alert1Minutes});
    if (m_alert2Minutes >= 0)
        alerts.append({2, m_alert2Minutes});
    if (alerts.isEmpty())
        return;
    std::sort(alerts.begin(), alerts.end(), [](const Alert &a, const Alert &b) { return a.minutes < b.minutes; });

    QSet<QString> firedThisPass;
    for (const Alert &alert : alerts) {
        const QDateTime horizon = now.addSecs(alert.minutes * 60);
        const QList<EventRow> due =
            m_db.upcomingUnnotified(now.toString(Qt::ISODate), horizon.toString(Qt::ISODate), alert.slot);
        for (const EventRow &event : due) {
            const QString key = event.href + QLatin1Char('|') + event.calendarId + QLatin1Char('|') + event.startsAtUtc;
            if (!firedThisPass.contains(key)) {
                const QDateTime starts = QDateTime::fromString(event.startsAtUtc, Qt::ISODate).toLocalTime();
                QString body = starts.isValid() ? tr("at %1").arg(starts.toString("HH:mm")) : QString();
                if (!event.location.isEmpty())
                    body += QStringLiteral(" · %1").arg(event.location);

                emit eventNotification(event.summary, body);
                firedThisPass.insert(key);
            }
            m_db.markNotified(event.href, event.calendarId, event.startsAtUtc, alert.slot);
        }
    }
}
