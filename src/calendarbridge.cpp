#include "calendarbridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSettings>
#include <QDebug>
#include <algorithm>
#include <memory>

#include "config.h"
#include "googleauth.h"
#include "keychain.h"
#include "platform.h"

CalendarBridge::CalendarBridge(QObject *parent) : QObject(parent)
{
    m_db.open(Database::defaultPath());
}

QString CalendarBridge::eventsJson(const QString &fromIso, const QString &toIso) const
{
    QJsonArray arr;
    for (const EventRow &e : m_db.listEventsInRange(fromIso, toIso)) {
        QJsonObject obj;
        obj["uid"] = e.uid;
        obj["href"] = e.href;
        obj["calendarId"] = e.calendarId;
        obj["summary"] = e.summary;
        obj["description"] = e.description;
        obj["location"] = e.location;
        obj["startsAt"] = e.startsAtUtc;
        obj["endsAt"] = e.endsAtUtc;
        obj["allDay"] = e.allDay;
        obj["color"] = e.color;
        obj["calendarName"] = e.calendarName;
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString CalendarBridge::calendarsJson() const
{
    QJsonArray arr;
    for (const CalendarRow &c : m_db.listCalendars()) {
        QJsonObject obj;
        obj["id"] = c.id;
        obj["accountId"] = c.accountId;
        obj["accountLabel"] = c.accountLabel;
        obj["displayName"] = c.displayName;
        obj["color"] = c.color;
        obj["visible"] = c.visible;
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void CalendarBridge::setCalendarVisible(const QString &calendarId, bool visible)
{
    // Purely local, synchronous, no network - safe to do directly here
    // rather than routing through SyncWorker.
    m_db.setCalendarVisible(calendarId, visible);
    emit eventsChanged();
}

void CalendarBridge::syncNow()
{
    emit requestSyncNow();
}

void CalendarBridge::createEvent(const QString &calendarId, const QString &summary, const QString &startsIso,
                                  const QString &endsIso, const QString &location, const QString &description)
{
    if (summary.trimmed().isEmpty())
        return;
    emit requestCreateEvent(calendarId, summary.trimmed(), startsIso, endsIso, location.trimmed(),
                             description.trimmed());
}

void CalendarBridge::deleteEvent(const QString &calendarId, const QString &href)
{
    if (href.isEmpty())
        return;
    emit requestDeleteEvent(calendarId, href);
}

void CalendarBridge::updateEvent(const QString &calendarId, const QString &href, const QString &uid,
                                  const QString &summary, const QString &startsIso, const QString &endsIso,
                                  const QString &location, const QString &description)
{
    if (href.isEmpty() || summary.trimmed().isEmpty())
        return;
    emit requestUpdateEvent(calendarId, href, uid, summary.trimmed(), startsIso, endsIso, location.trimmed(),
                             description.trimmed());
}

int CalendarBridge::alert1Minutes() const
{
    QSettings settings;
    return settings.value("reminders/alert1Minutes", 10).toInt();
}

int CalendarBridge::alert2Minutes() const
{
    QSettings settings;
    return settings.value("reminders/alert2Minutes", -1).toInt();
}

void CalendarBridge::setReminderMinutes(int alert1, int alert2)
{
    QSettings settings;
    settings.setValue("reminders/alert1Minutes", alert1);
    settings.setValue("reminders/alert2Minutes", alert2);
    emit reminderOffsetsChanged(alert1, alert2);
}

void CalendarBridge::onSyncFinished(bool syncing, const QString &lastError)
{
    m_syncing = syncing;
    m_lastError = lastError;
    emit syncStatusChanged();
    if (!syncing) {
        // A sync pass (or event creation) just finished - the local
        // cache may have changed, tell QML to re-read it.
        emit eventsChanged();
    }
}

int CalendarBridge::firstDayOfWeek() const
{
    QSettings settings;
    return settings.value("ui/firstDayOfWeek", -1).toInt();
}

void CalendarBridge::setFirstDayOfWeek(int day)
{
    if (day == firstDayOfWeek())
        return;
    QSettings settings;
    settings.setValue("ui/firstDayOfWeek", day);
    emit firstDayOfWeekChanged();
}

QString CalendarBridge::accountsJson() const
{
    QJsonArray arr;
    for (const AccountConfig &a : m_db.listAccounts()) {
        QJsonObject obj;
        obj["id"] = a.id;
        obj["kind"] = a.kind;
        obj["label"] = a.label;
        obj["username"] = a.username;
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void CalendarBridge::addAccount(const QString &kind, const QString &label, const QString &url,
                                const QString &username, const QString &password)
{
    if (username.trimmed().isEmpty() || password.isEmpty()) {
        emit accountAddResult(false, tr("Enter your username and password."));
        return;
    }
    emit requestVerifyAccount(kind, label, url, username, password);
}

void CalendarBridge::onAccountVerified(const AccountConfig &account, int calendarCount)
{
    // Credentials work - only now does anything get persisted, password
    // first: an account row without a retrievable password would be a
    // broken account on the next start.
    Keychain::writePassword(account.id, account.password, this,
                            [this, account, calendarCount](bool ok, const QString &error) {
        if (!ok) {
            emit accountAddResult(false, tr("The server accepted the password, but it couldn't be saved to the "
                                            "system password store (KWallet / Secret Service): %1")
                                             .arg(error));
            return;
        }
        emit requestCommitAccount(account);
        emit accountAddResult(true, tr("Connected “%1”. Calendars found: %2.")
                                         .arg(account.label)
                                         .arg(calendarCount));
    });
}

void CalendarBridge::onAccountVerifyFailed(const QString &message)
{
    emit accountAddResult(false, message);
}

void CalendarBridge::removeAccount(const QString &accountId)
{
    m_accountError.clear(); // most likely the user is removing exactly the broken account
    Keychain::deletePassword(accountId);
    emit requestRemoveAccount(accountId);
}

void CalendarBridge::onAccountsChanged()
{
    emit accountsChanged();
    emit eventsChanged();
}

void CalendarBridge::loadAccounts()
{
    QList<AccountConfig> accounts = m_db.listAccounts();

    // One-time migration from the pre-UI setup: accounts.json (+ passwords
    // via secret-tool). Imported accounts get their password copied into
    // the keychain, and the file is renamed so this never runs twice.
    const QString legacyPath = Config::configPath();
    if (QFile::exists(legacyPath)) {
        for (const AccountConfig &legacy : Config::loadAccounts()) {
            const bool known = std::any_of(accounts.cbegin(), accounts.cend(),
                                           [&](const AccountConfig &a) { return a.id == legacy.id; });
            if (!known) {
                m_db.upsertAccount(legacy.id, legacy.kind, legacy.label, legacy.baseUrl, legacy.username);
                accounts.append(legacy);
            }
        }
        QFile::rename(legacyPath, legacyPath + QStringLiteral(".imported"));
        qInfo() << "Imported legacy accounts.json; renamed to accounts.json.imported";
    }

    if (accounts.isEmpty()) {
        emit requestSetAccounts({});
        return;
    }

    // Read every password asynchronously; hand the full set to the worker
    // once the last read finishes.
    struct Pending {
        int remaining = 0;
        QList<AccountConfig> ready;
        QStringList missing;
    };
    auto state = std::make_shared<Pending>();
    state->remaining = accounts.size();

    auto finishOne = [this, state]() {
        if (--state->remaining > 0)
            return;
        if (!state->missing.isEmpty()) {
            m_accountError = tr("No saved password for: %1. Remove this account and add it again.")
                              .arg(state->missing.join(", "));
            emit syncStatusChanged();
        }
        emit requestSetAccounts(state->ready);
    };

    for (const AccountConfig &account : accounts) {
        Keychain::readPassword(account.id, this,
                               [this, account, state, finishOne](bool ok, const QString &password, const QString &) {
            AccountConfig withPassword = account;
            if (ok && !password.isEmpty()) {
                withPassword.password = password;
            } else {
                // Not in the keychain yet: the account predates QtKeychain.
                // Pick up its secret-tool password (if any) and move it over.
                withPassword.password = Config::legacyLookupPassword(account.username);
                if (!withPassword.password.isEmpty())
                    Keychain::writePassword(account.id, withPassword.password, this, [](bool, const QString &) {});
            }
            if (withPassword.password.isEmpty())
                state->missing.append(account.label);
            else
                state->ready.append(withPassword);
            finishOne();
        });
    }
}

QString CalendarBridge::language() const
{
    return QSettings().value("ui/language").toString();
}

void CalendarBridge::setLanguage(const QString &language)
{
    if (language == this->language())
        return;
    QSettings().setValue("ui/language", language);
    emit languageChanged();
}

bool CalendarBridge::autostart() const
{
    return Autostart::isEnabled();
}

void CalendarBridge::setAutostart(bool enabled)
{
    Autostart::setEnabled(enabled, tr("Show event reminders while the window is closed."), this,
                          [this, enabled](bool ok, bool enabledNow, const QString &error) {
        if (ok && enabledNow == enabled) {
            m_platformError.clear();
        } else {
            qWarning() << "Autostart change failed:" << error;
            m_platformError = enabled ? tr("The system didn't allow starting automatically at login.")
                                      : tr("Couldn't turn off starting at login.");
        }
        emit autostartChanged();
        emit syncStatusChanged(); // lastError may have changed
    });
}

bool CalendarBridge::googleAvailable() const
{
    return GoogleConfig::isConfigured();
}

void CalendarBridge::addGoogleAccount(const QString &label)
{
    cancelGoogleSignIn(); // at most one attempt at a time

    m_googleSignIn = new GoogleSignIn(this);
    connect(m_googleSignIn, &GoogleSignIn::openBrowserRequested, this, &CalendarBridge::openUrlRequested);
    connect(m_googleSignIn, &GoogleSignIn::finished, this,
            [this, label](bool ok, const QString &email, const QString &refreshToken, const QString &error) {
        if (m_googleSignIn)
            m_googleSignIn->deleteLater();
        if (!ok) {
            emit accountAddResult(false, error);
            return;
        }
        for (const AccountConfig &existing : m_db.listAccounts()) {
            if (existing.kind == QLatin1String("google") && existing.username.compare(email, Qt::CaseInsensitive) == 0) {
                emit accountAddResult(false, tr("%1 is already connected.").arg(email));
                return;
            }
        }
        emit googleSignedIn();
        // From here on it's an ordinary account: the refresh token takes
        // the place of a password (verified on the worker, then stored in
        // the keychain by onAccountVerified).
        emit requestVerifyAccount(QStringLiteral("google"), label, QString(), email, refreshToken);
    });
    m_googleSignIn->start();
}

void CalendarBridge::cancelGoogleSignIn()
{
    if (m_googleSignIn) {
        m_googleSignIn->disconnect(this);
        m_googleSignIn->deleteLater();
        m_googleSignIn = nullptr;
    }
}
