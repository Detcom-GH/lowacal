#pragma once

#include <QObject>
#include <QDateTime>
#include <QHash>
#include <QTimer>
#include <memory>

#include "account.h"
#include "caldavclient.h"
#include "database.h"

// Runs entirely on its own QThread (see main.cpp): discovers calendars,
// re-fetches events for any calendar whose ctag changed, checks for
// upcoming events to notify about, and handles "create a new event"
// requests. Never touches a QML/GUI object directly - talks back to
// CalendarBridge purely via signals, which Qt automatically marshals
// across the thread boundary.
class SyncWorker : public QObject {
    Q_OBJECT

public:
    explicit SyncWorker(QObject *parent = nullptr);

public slots:
    // Connected to QThread::started - this is where setup that must
    // happen *on* the worker thread (opening its own SQLite connection,
    // starting timers) belongs, rather than in the constructor (which
    // runs on whichever thread creates the SyncWorker, before moveToThread).
    void start();

    // Invoked (queued, on the worker thread) right before the thread is
    // asked to quit - stops the timers from the thread that owns them,
    // which is the only thread allowed to (Qt warns loudly otherwise).
    void shutdown();

    void requestSyncNow();
    void requestCreateEvent(const QString &calendarId, const QString &summary, const QString &startsIso,
                             const QString &endsIso, const QString &location, const QString &description);
    void requestDeleteEvent(const QString &calendarId, const QString &href);
    void requestUpdateEvent(const QString &calendarId, const QString &href, const QString &uid,
                             const QString &summary, const QString &startsIso, const QString &endsIso,
                             const QString &location, const QString &description);
    // Minutes before an event to fire each alert; <0 disables that slot.
    // Applied globally to every event (per-event VALARM parsing is a
    // later feature) - see CalendarBridge for where these come from.
    void setReminderOffsets(int alert1Minutes, int alert2Minutes);

    // --- account management ---
    // Accounts (with passwords already read from the keychain) are pushed
    // here by CalendarBridge, which owns keychain access on the GUI
    // thread. Replaces whatever set was active and syncs.
    void setAccounts(const QList<AccountConfig> &accounts);
    // Step 1 of adding an account: run CalDAV discovery with the given
    // credentials WITHOUT saving anything. Emits accountVerified (with
    // the resolved endpoint filled in) or accountVerifyFailed (with a
    // message meant for the user, not a log).
    void verifyAccount(const QString &kind, const QString &label, const QString &url, const QString &username,
                       const QString &password);
    // Step 2, after the GUI thread stored the password in the keychain:
    // persist the account, start syncing it.
    void commitAccount(const AccountConfig &account);
    void removeAccount(const QString &accountId);

signals:
    // Queued automatically to CalendarBridge::onSyncFinished on the GUI
    // thread.
    void syncStatusChanged(bool syncing, const QString &lastError);

    // Queued automatically to TrayController, which is the only thing
    // allowed to touch the actual QSystemTrayIcon (a GUI-thread-only
    // object) - see main.cpp for the connection.
    void eventNotification(const QString &title, const QString &body);

    void accountVerified(const AccountConfig &account, int calendarCount);
    void accountVerifyFailed(const QString &message);
    void accountsChanged();

private:
    void runFullSync();
    void checkNotifications();
    bool syncAccount(const AccountConfig &account, const QString &fromIcal, const QString &toIcal,
                     QString *error = nullptr);
    // A CalDAV client authenticated for this account: Basic auth for
    // iCloud/CalDAV, a fresh OAuth access token for Google (obtained from
    // the stored refresh token, cached until shortly before it expires).
    // Returns null and fills `error` (a message for the user) when a
    // Google token can't be obtained.
    std::unique_ptr<CalDavClient> clientFor(const AccountConfig &account, QString *error);
    // Who are we on this server? Standard discovery, plus Google's
    // documented principal address as a fallback for Google accounts.
    QString principalFor(CalDavClient &client, const AccountConfig &account, bool *ok);
    void doCreateEvent(const QString &calendarId, const QString &summary, const QString &startsIso,
                        const QString &endsIso, const QString &location, const QString &description);
    const AccountConfig *accountForCalendar(const QString &calendarId, QString *calendarHref = nullptr) const;

    Database m_db{"worker_connection"};
    QList<AccountConfig> m_accounts;
    QTimer *m_syncTimer = nullptr;
    QTimer *m_notifyTimer = nullptr;
    struct AccessToken {
        QString token;
        QDateTime expires;
    };
    QHash<QString, AccessToken> m_googleTokens; // by refresh token; memory only
    int m_alert1Minutes = 10;
    int m_alert2Minutes = -1; // disabled by default
};
