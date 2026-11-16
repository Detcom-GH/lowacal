#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QString>

#include "account.h"
#include "database.h"

// The QML-facing surface of the app. Registered in main.cpp as a context
// property ("CalendarBridge"), so QML uses it directly with no import
// needed. Every Q_INVOKABLE here is synchronous and reads straight from
// SQLite (fast, local, never touches the network) - real work (CalDAV
// sync, ICS parsing, notifications) happens on SyncWorker's dedicated
// thread and reaches this object only through queued signals, which Qt
// marshals onto the GUI thread automatically and safely.
//
// Unlike the earlier Rust/cxx-qt version of this app, QML does *not*
// need to poll this object on a timer to notice new data - eventsChanged()
// is a real, GUI-thread-safe signal fired from onSyncFinished(), so
// property bindings and Connections{} in QML update immediately. Getting
// this right was one of the concrete advantages of using Qt's native
// threading directly instead of guessing at an unfamiliar Rust-side
// cross-thread API.
class CalendarBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncStatusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY syncStatusChanged)
    // -1 = follow the system locale; otherwise 0 = Sunday, 1 = Monday
    // (same numbering as QML's Locale.firstDayOfWeek / JS Date.getDay()).
    Q_PROPERTY(int firstDayOfWeek READ firstDayOfWeek WRITE setFirstDayOfWeek NOTIFY firstDayOfWeekChanged)
    // "" = follow the system, "en", "uk". Saved immediately, applied on the
    // next launch (see setupLanguage in appwiring.h).
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    // Launch at login, into the tray. Changed via setAutostart(), which is
    // asynchronous inside Flatpak (the system may ask the user first).
    Q_PROPERTY(bool autostart READ autostart NOTIFY autostartChanged)
    // False when this build has no Google OAuth client configured (see
    // GoogleConfig) - the Google option is then shown as unavailable.
    Q_PROPERTY(bool googleAvailable READ googleAvailable CONSTANT)

public:
    explicit CalendarBridge(QObject *parent = nullptr);

    bool syncing() const { return m_syncing; }
    // A sync error (transient, cleared by the next good sync) wins; an
    // account-setup problem (e.g. a password missing from the keychain)
    // is shown otherwise, and is NOT wiped by routine sync status updates.
    QString lastError() const
    {
        if (!m_lastError.isEmpty())
            return m_lastError;
        return m_accountError.isEmpty() ? m_platformError : m_accountError;
    }
    int firstDayOfWeek() const;
    void setFirstDayOfWeek(int day);
    QString language() const;
    void setLanguage(const QString &language);
    bool autostart() const;
    bool googleAvailable() const;

    // Reads saved accounts + their keychain passwords (migrating any
    // legacy accounts.json / secret-tool setup on the way), then hands
    // them to SyncWorker. Call once, after all connections are made.
    void loadAccounts();

    Q_INVOKABLE QString eventsJson(const QString &fromIso, const QString &toIso) const;
    Q_INVOKABLE QString calendarsJson() const;
    Q_INVOKABLE void setCalendarVisible(const QString &calendarId, bool visible);

    // Fire-and-forget: queues the request on SyncWorker's thread and
    // returns immediately. Results show up via eventsChanged()/
    // syncStatusChanged() once the worker finishes, not synchronously.
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void createEvent(const QString &calendarId, const QString &summary, const QString &startsIso,
                                  const QString &endsIso, const QString &location, const QString &description);
    Q_INVOKABLE void deleteEvent(const QString &calendarId, const QString &href);
    Q_INVOKABLE void updateEvent(const QString &calendarId, const QString &href, const QString &uid,
                                  const QString &summary, const QString &startsIso, const QString &endsIso,
                                  const QString &location, const QString &description);

    // Global reminder offsets (minutes before an event; -1 = disabled).
    // Persisted via QSettings, applied live to the running worker.
    Q_INVOKABLE int alert1Minutes() const;
    Q_INVOKABLE int alert2Minutes() const;
    Q_INVOKABLE void setReminderMinutes(int alert1, int alert2);

    // --- accounts ---
    Q_INVOKABLE QString accountsJson() const;
    // Verifies the credentials against the server first; nothing is saved
    // unless that succeeds. Result arrives via accountAddResult().
    Q_INVOKABLE void addAccount(const QString &kind, const QString &label, const QString &url,
                                const QString &username, const QString &password);
    Q_INVOKABLE void removeAccount(const QString &accountId);
    // Google: sign-in happens in the user's browser (OAuth), then the same
    // verify -> keychain -> commit path as other accounts. The result
    // arrives via accountAddResult() too.
    Q_INVOKABLE void addGoogleAccount(const QString &label);
    Q_INVOKABLE void cancelGoogleSignIn();

    Q_INVOKABLE void setAutostart(bool enabled);

signals:
    void eventsChanged();
    void syncStatusChanged();

    // Connected (auto-queued, cross-thread) to SyncWorker's matching slots.
    void requestSyncNow();
    void requestCreateEvent(const QString &calendarId, const QString &summary, const QString &startsIso,
                             const QString &endsIso, const QString &location, const QString &description);
    void requestDeleteEvent(const QString &calendarId, const QString &href);
    void requestUpdateEvent(const QString &calendarId, const QString &href, const QString &uid,
                             const QString &summary, const QString &startsIso, const QString &endsIso,
                             const QString &location, const QString &description);
    void reminderOffsetsChanged(int alert1Minutes, int alert2Minutes);
    void firstDayOfWeekChanged();
    void languageChanged();
    void autostartChanged();
    void accountsChanged();
    void accountAddResult(bool ok, const QString &message);
    // The app opens this in the user's browser (main.cpp); tests play the browser.
    void openUrlRequested(const QUrl &url);
    // Browser part finished successfully; now checking calendar access.
    void googleSignedIn();

    void requestSetAccounts(const QList<AccountConfig> &accounts);
    void requestVerifyAccount(const QString &kind, const QString &label, const QString &url,
                              const QString &username, const QString &password);
    void requestCommitAccount(const AccountConfig &account);
    void requestRemoveAccount(const QString &accountId);

public slots:
    // Connected (auto-queued) to SyncWorker::syncStatusChanged.
    void onSyncFinished(bool syncing, const QString &lastError);
    void onAccountVerified(const AccountConfig &account, int calendarCount);
    void onAccountVerifyFailed(const QString &message);
    void onAccountsChanged();

private:
    bool m_syncing = false;
    QString m_lastError;
    QString m_accountError;
    QPointer<class GoogleSignIn> m_googleSignIn;
    QString m_platformError; // e.g. the system refused launch-at-login
    mutable Database m_db{"gui_connection"};
};
