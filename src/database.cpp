#include "database.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QDebug>

Database::Database(const QString &connectionName) : m_connectionName(connectionName) {}

Database::~Database()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
    // Drop our reference before removeDatabase, which warns if a
    // QSqlDatabase object still refers to the connection name it's
    // asked to remove - m_db itself is that reference until this runs.
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

QString Database::defaultPath()
{
    // Deliberately GenericDataLocation (~/.local/share, no automatic
    // nesting) with an explicit "/lowacal" appended, rather than
    // AppDataLocation - which on Linux nests as
    // <organizationName>/<applicationName>, and since main.cpp sets both
    // to "lowacal", that silently produced
    // ~/.local/share/lowacal/lowacal/cache.db instead of the
    // ~/.local/share/lowacal/cache.db documented in the README. Being
    // explicit here means the path is whatever this line says, full
    // stop - no implicit Qt behavior to be wrong about a second time.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return base + "/lowacal/cache.db";
}

bool Database::open(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        qWarning() << "Database: failed to open" << path << m_db.lastError().text();
        return false;
    }

    QSqlQuery pragma(m_db);
    pragma.exec("PRAGMA journal_mode = WAL");
    pragma.exec("PRAGMA foreign_keys = ON");

    return applySchema();
}

bool Database::applySchema()
{
    static const QStringList statements = {
        R"(CREATE TABLE IF NOT EXISTS accounts (
            id              TEXT PRIMARY KEY,
            kind            TEXT NOT NULL,
            label           TEXT NOT NULL,
            caldav_base_url TEXT,
            username        TEXT
        ))",
        R"(CREATE TABLE IF NOT EXISTS calendars (
            id              TEXT PRIMARY KEY,
            account_id      TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
            href            TEXT NOT NULL,
            display_name    TEXT NOT NULL,
            color           TEXT NOT NULL DEFAULT '#3DAEE9',
            ctag            TEXT,
            visible         INTEGER NOT NULL DEFAULT 1,
            read_only       INTEGER NOT NULL DEFAULT 0
        ))",
        R"(CREATE TABLE IF NOT EXISTS events (
            href            TEXT NOT NULL,
            calendar_id     TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
            uid             TEXT NOT NULL DEFAULT '',
            summary         TEXT NOT NULL DEFAULT '',
            description     TEXT,
            location        TEXT,
            starts_at_utc   TEXT NOT NULL,
            ends_at_utc     TEXT NOT NULL,
            all_day         INTEGER NOT NULL DEFAULT 0,
            rrule           TEXT,
            PRIMARY KEY (href, calendar_id)
        ))",
        "CREATE INDEX IF NOT EXISTS idx_events_range ON events (starts_at_utc, ends_at_utc)",
        R"(CREATE TABLE IF NOT EXISTS notified_events (
            href            TEXT NOT NULL,
            calendar_id     TEXT NOT NULL,
            starts_at_utc   TEXT NOT NULL,
            alert_slot      INTEGER NOT NULL DEFAULT 1,
            PRIMARY KEY (href, calendar_id, starts_at_utc, alert_slot)
        ))",
    };

    for (const QString &sql : statements) {
        QSqlQuery q(m_db);
        if (!q.exec(sql)) {
            qWarning() << "Database: schema statement failed:" << q.lastError().text();
            return false;
        }
    }

    // Cache generation. The cache is only a copy of what the servers hold,
    // so when the way events are parsed changes, the cheap and safe fix
    // is to forget the ctags: the next sync then re-reads every calendar.
    //   2: event times honour TZID / floating time (were read as UTC)
    constexpr int kCacheGeneration = 2;
    QSqlQuery version(m_db);
    if (version.exec("PRAGMA user_version") && version.next() && version.value(0).toInt() < kCacheGeneration) {
        version.finish();
        QSqlQuery reset(m_db);
        reset.exec("UPDATE calendars SET ctag = NULL");
        reset.exec(QStringLiteral("PRAGMA user_version = %1").arg(kCacheGeneration));
    }
    return true;
}

bool Database::upsertAccount(const QString &id, const QString &kind, const QString &label,
                              const QString &baseUrl, const QString &username)
{
    QSqlQuery q(m_db);
    q.prepare(R"(INSERT INTO accounts (id, kind, label, caldav_base_url, username)
                 VALUES (:id, :kind, :label, :baseUrl, :username)
                 ON CONFLICT(id) DO UPDATE SET
                    kind = excluded.kind, label = excluded.label,
                    caldav_base_url = excluded.caldav_base_url, username = excluded.username)");
    q.bindValue(":id", id);
    q.bindValue(":kind", kind);
    q.bindValue(":label", label);
    q.bindValue(":baseUrl", baseUrl);
    q.bindValue(":username", username);
    if (!q.exec()) {
        qWarning() << "upsertAccount failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::upsertCalendar(const QString &id, const QString &accountId, const QString &href,
                               const QString &displayName, const QString &color, const QString &ctag)
{
    QSqlQuery q(m_db);
    q.prepare(R"(INSERT INTO calendars (id, account_id, href, display_name, color, ctag)
                 VALUES (:id, :accountId, :href, :displayName, :color, :ctag)
                 ON CONFLICT(id) DO UPDATE SET
                    href = excluded.href, display_name = excluded.display_name,
                    color = excluded.color, ctag = excluded.ctag)");
    q.bindValue(":id", id);
    q.bindValue(":accountId", accountId);
    q.bindValue(":href", href);
    q.bindValue(":displayName", displayName);
    q.bindValue(":color", color);
    q.bindValue(":ctag", ctag.isEmpty() ? QVariant() : QVariant(ctag));
    if (!q.exec()) {
        qWarning() << "upsertCalendar failed:" << q.lastError().text();
        return false;
    }
    return true;
}

QList<AccountConfig> Database::listAccounts()
{
    QList<AccountConfig> out;
    QSqlQuery q(m_db);
    if (!q.exec("SELECT id, kind, label, caldav_base_url, username FROM accounts ORDER BY label")) {
        qWarning() << "listAccounts failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        AccountConfig a;
        a.id = q.value(0).toString();
        a.kind = q.value(1).toString();
        a.label = q.value(2).toString();
        a.baseUrl = q.value(3).toString();
        a.username = q.value(4).toString();
        out.append(a);
    }
    return out;
}

bool Database::deleteAccount(const QString &accountId)
{
    // Explicit deletes rather than relying on ON DELETE CASCADE alone -
    // notified_events has no foreign key, and being explicit means this
    // doesn't silently depend on PRAGMA foreign_keys being on for
    // whichever connection happens to call it.
    if (!m_db.transaction())
        return false;
    const QString calPattern = accountId + QStringLiteral(":%");
    const QStringList statements = {
        QStringLiteral("DELETE FROM notified_events WHERE calendar_id LIKE :pat"),
        QStringLiteral("DELETE FROM events WHERE calendar_id LIKE :pat"),
        QStringLiteral("DELETE FROM calendars WHERE account_id = :id"),
        QStringLiteral("DELETE FROM accounts WHERE id = :id"),
    };
    for (const QString &sql : statements) {
        QSqlQuery q(m_db);
        q.prepare(sql);
        if (sql.contains(":pat"))
            q.bindValue(":pat", calPattern);
        else
            q.bindValue(":id", accountId);
        if (!q.exec()) {
            qWarning() << "deleteAccount failed:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    return m_db.commit();
}

QString Database::calendarCtag(const QString &calendarId)
{
    QSqlQuery q(m_db);
    q.prepare("SELECT ctag FROM calendars WHERE id = :id");
    q.bindValue(":id", calendarId);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return QString();
}

bool Database::setCalendarVisible(const QString &calendarId, bool visible)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE calendars SET visible = :visible WHERE id = :id");
    q.bindValue(":visible", visible ? 1 : 0);
    q.bindValue(":id", calendarId);
    return q.exec();
}

QList<CalendarRow> Database::listCalendars()
{
    QList<CalendarRow> out;
    QSqlQuery q(m_db);
    q.prepare(R"(SELECT c.id, c.account_id, a.label, c.display_name, c.color, c.visible
                 FROM calendars c JOIN accounts a ON a.id = c.account_id
                 ORDER BY a.label, c.display_name)");
    if (!q.exec()) {
        qWarning() << "listCalendars failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        CalendarRow row;
        row.id = q.value(0).toString();
        row.accountId = q.value(1).toString();
        row.accountLabel = q.value(2).toString();
        row.displayName = q.value(3).toString();
        row.color = q.value(4).toString();
        row.visible = q.value(5).toInt() != 0;
        out.append(row);
    }
    return out;
}

bool Database::replaceEventsForCalendar(const QString &calendarId, const QList<EventRow> &events)
{
    if (!m_db.transaction()) {
        qWarning() << "replaceEventsForCalendar: could not start transaction";
        return false;
    }

    QSqlQuery del(m_db);
    del.prepare("DELETE FROM events WHERE calendar_id = :id");
    del.bindValue(":id", calendarId);
    if (!del.exec()) {
        qWarning() << "replaceEventsForCalendar: delete failed:" << del.lastError().text();
        m_db.rollback();
        return false;
    }

    for (const EventRow &e : events) {
        QSqlQuery ins(m_db);
        ins.prepare(R"(INSERT INTO events
                        (href, calendar_id, uid, summary, description, location, starts_at_utc, ends_at_utc, all_day)
                       VALUES (:href, :cal, :uid, :summary, :description, :location, :starts, :ends, :allDay)
                       ON CONFLICT(href, calendar_id) DO UPDATE SET
                          uid = excluded.uid, summary = excluded.summary, description = excluded.description,
                          location = excluded.location, starts_at_utc = excluded.starts_at_utc,
                          ends_at_utc = excluded.ends_at_utc, all_day = excluded.all_day)");
        ins.bindValue(":href", e.href);
        ins.bindValue(":cal", calendarId);
        ins.bindValue(":uid", e.uid);
        ins.bindValue(":summary", e.summary);
        ins.bindValue(":description", e.description.isEmpty() ? QVariant() : QVariant(e.description));
        ins.bindValue(":location", e.location.isEmpty() ? QVariant() : QVariant(e.location));
        ins.bindValue(":starts", e.startsAtUtc);
        ins.bindValue(":ends", e.endsAtUtc);
        ins.bindValue(":allDay", e.allDay ? 1 : 0);
        if (!ins.exec()) {
            qWarning() << "replaceEventsForCalendar: insert failed:" << ins.lastError().text();
            m_db.rollback();
            return false;
        }
    }

    return m_db.commit();
}

bool Database::upsertEvent(const EventRow &e)
{
    QSqlQuery q(m_db);
    q.prepare(R"(INSERT INTO events
                    (href, calendar_id, uid, summary, description, location, starts_at_utc, ends_at_utc, all_day)
                 VALUES (:href, :cal, :uid, :summary, :description, :location, :starts, :ends, :allDay)
                 ON CONFLICT(href, calendar_id) DO UPDATE SET
                    uid = excluded.uid, summary = excluded.summary, description = excluded.description,
                    location = excluded.location, starts_at_utc = excluded.starts_at_utc,
                    ends_at_utc = excluded.ends_at_utc, all_day = excluded.all_day)");
    q.bindValue(":href", e.href);
    q.bindValue(":cal", e.calendarId);
    q.bindValue(":uid", e.uid);
    q.bindValue(":summary", e.summary);
    q.bindValue(":description", e.description.isEmpty() ? QVariant() : QVariant(e.description));
    q.bindValue(":location", e.location.isEmpty() ? QVariant() : QVariant(e.location));
    q.bindValue(":starts", e.startsAtUtc);
    q.bindValue(":ends", e.endsAtUtc);
    q.bindValue(":allDay", e.allDay ? 1 : 0);
    if (!q.exec()) {
        qWarning() << "upsertEvent failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::deleteEvent(const QString &href, const QString &calendarId)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM events WHERE href = :href AND calendar_id = :cal");
    q.bindValue(":href", href);
    q.bindValue(":cal", calendarId);
    if (!q.exec()) {
        qWarning() << "deleteEvent failed:" << q.lastError().text();
        return false;
    }
    return true;
}

QList<EventRow> Database::listEventsInRange(const QString &fromIso, const QString &toIso)
{
    QList<EventRow> out;
    QSqlQuery q(m_db);
    q.prepare(R"(SELECT e.uid, e.calendar_id, e.summary, e.description, e.location,
                        e.starts_at_utc, e.ends_at_utc, e.all_day, c.color, c.display_name, e.href
                 FROM events e JOIN calendars c ON c.id = e.calendar_id
                 WHERE c.visible = 1 AND e.starts_at_utc < :to AND e.ends_at_utc > :from
                 ORDER BY e.starts_at_utc)");
    q.bindValue(":from", fromIso);
    q.bindValue(":to", toIso);
    if (!q.exec()) {
        qWarning() << "listEventsInRange failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        EventRow row;
        row.uid = q.value(0).toString();
        row.calendarId = q.value(1).toString();
        row.summary = q.value(2).toString();
        row.description = q.value(3).toString();
        row.location = q.value(4).toString();
        row.startsAtUtc = q.value(5).toString();
        row.endsAtUtc = q.value(6).toString();
        row.allDay = q.value(7).toInt() != 0;
        row.color = q.value(8).toString();
        row.calendarName = q.value(9).toString();
        row.href = q.value(10).toString();
        out.append(row);
    }
    return out;
}

QList<EventRow> Database::upcomingUnnotified(const QString &nowIso, const QString &horizonIso, int alertSlot)
{
    QList<EventRow> out;
    QSqlQuery q(m_db);
    q.prepare(R"(SELECT e.uid, e.calendar_id, e.summary, e.description, e.location,
                        e.starts_at_utc, e.ends_at_utc, e.all_day, e.href
                 FROM events e JOIN calendars c ON c.id = e.calendar_id
                 WHERE c.visible = 1 AND e.all_day = 0
                   AND e.starts_at_utc BETWEEN :now AND :horizon
                   AND NOT EXISTS (
                       SELECT 1 FROM notified_events n
                       WHERE n.href = e.href AND n.calendar_id = e.calendar_id
                         AND n.starts_at_utc = e.starts_at_utc AND n.alert_slot = :slot
                   )
                 ORDER BY e.starts_at_utc)");
    q.bindValue(":now", nowIso);
    q.bindValue(":horizon", horizonIso);
    q.bindValue(":slot", alertSlot);
    if (!q.exec()) {
        qWarning() << "upcomingUnnotified failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        EventRow row;
        row.uid = q.value(0).toString();
        row.calendarId = q.value(1).toString();
        row.summary = q.value(2).toString();
        row.description = q.value(3).toString();
        row.location = q.value(4).toString();
        row.startsAtUtc = q.value(5).toString();
        row.endsAtUtc = q.value(6).toString();
        row.allDay = q.value(7).toInt() != 0;
        row.href = q.value(8).toString();
        out.append(row);
    }
    return out;
}

bool Database::markNotified(const QString &href, const QString &calendarId, const QString &startsAtUtc, int alertSlot)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT OR IGNORE INTO notified_events (href, calendar_id, starts_at_utc, alert_slot) "
              "VALUES (:href, :cal, :starts, :slot)");
    q.bindValue(":href", href);
    q.bindValue(":cal", calendarId);
    q.bindValue(":starts", startsAtUtc);
    q.bindValue(":slot", alertSlot);
    return q.exec();
}
