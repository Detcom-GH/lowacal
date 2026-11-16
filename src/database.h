#pragma once

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "account.h"

// The local cache. Everything the UI shows comes from here, never
// directly from the network - CalDavClient/SyncWorker are the only code
// that talks to CalDAV, and their job is purely to keep these tables up
// to date. That split is what lets CalendarBridge's queries stay instant
// even when the network is slow or offline.
//
// SQLite + QSqlDatabase note: a QSqlDatabase connection is not safe to
// share across threads. This app opens two: one on the GUI thread (for
// CalendarBridge's reads) and one on the worker thread (for SyncWorker's
// writes), both pointing at the same file, distinguished by connection
// name. WAL mode (enabled in open()) is what makes concurrent access
// from both safe.

struct CalendarRow {
    QString id;
    QString accountId; // not currently read anywhere - reserved for a future account-grouping view
    QString accountLabel;
    QString displayName;
    QString color;
    bool visible = true;
};

struct EventRow {
    QString uid; // informational only now - see href below for why this isn't the key
    QString href; // the actual uniqueness key: server-guaranteed unique per resource,
                  // unlike UID, which real-world data has shown can collide across
                  // genuinely distinct events (see README)
    QString calendarId;
    QString summary;
    QString description; // empty = none
    QString location; // empty = none
    QString startsAtUtc; // ISO-8601 UTC
    QString endsAtUtc;
    bool allDay = false;
    // Only populated by listEventsInRange (joined from calendars) - left
    // blank on rows built while parsing ICS for insertion, where the
    // events table itself has no colour/name column of its own.
    QString color;
    QString calendarName;
};

class Database {
public:
    explicit Database(const QString &connectionName);
    ~Database();

    static QString defaultPath();

    bool open(const QString &path);

    bool upsertAccount(const QString &id, const QString &kind, const QString &label,
                        const QString &baseUrl, const QString &username);
    // Password field is always left empty - passwords live in the keychain.
    QList<AccountConfig> listAccounts();
    // Removes the account and everything under it (calendars, events,
    // notification bookkeeping) in one transaction.
    bool deleteAccount(const QString &accountId);

    bool upsertCalendar(const QString &id, const QString &accountId, const QString &href,
                         const QString &displayName, const QString &color, const QString &ctag);
    QString calendarCtag(const QString &calendarId);
    bool setCalendarVisible(const QString &calendarId, bool visible);
    QList<CalendarRow> listCalendars();

    bool replaceEventsForCalendar(const QString &calendarId, const QList<EventRow> &events);
    bool upsertEvent(const EventRow &event);
    bool deleteEvent(const QString &href, const QString &calendarId);
    QList<EventRow> listEventsInRange(const QString &fromIso, const QString &toIso);

    QList<EventRow> upcomingUnnotified(const QString &nowIso, const QString &horizonIso, int alertSlot);
    bool markNotified(const QString &href, const QString &calendarId, const QString &startsAtUtc, int alertSlot);

private:
    bool applySchema();

    QString m_connectionName;
    QSqlDatabase m_db;
};
