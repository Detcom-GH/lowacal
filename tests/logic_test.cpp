// Standalone smoke test for the trickiest, highest-risk logic in this
// app: ICS round-tripping and the SQLite schema/queries. Deliberately
// has zero QML/Kirigami/network dependency, so it builds and runs
// anywhere Qt6 does - including this sandbox, unlike the full GUI app.
//
// Not a full test suite (no QtTest framework, just plain asserts) -
// enough to catch the class of bug this project has already hit a few
// times (a format string subtly wrong, an escape rule backwards, a SQL
// column index off by one), verified against a real compiler and real
// SQLite rather than reasoned about from memory.

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTimeZone>
#include <QUuid>
#include <cstdio>

#include "caldavclient.h"
#include "config.h"
#include "database.h"
#include "icalparser.h"

namespace {
int failures = 0;

void check(bool condition, const char *description)
{
    if (condition) {
        std::printf("  ok   - %s\n", description);
    } else {
        std::printf("  FAIL - %s\n", description);
        ++failures;
    }
}
} // namespace

void testIcalRoundTrip()
{
    std::printf("-- ICS round-trip --\n");

    const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QDateTime starts(QDate(2026, 7, 10), QTime(10, 0), QTimeZone::utc());
    const QDateTime ends(QDate(2026, 7, 10), QTime(11, 30), QTimeZone::utc());
    const QString summary = "Огляд, коми; крапки з комою \\ і \"лапки\"";
    const QString location = "Кав'ярня «Аромат»";
    const QString description = "Перший рядок\nДругий рядок, з комою.";

    const QString ics = IcalParser::buildVEventIcs(uid, summary, starts, ends, location, description);
    check(ics.contains("BEGIN:VEVENT"), "generated ICS has BEGIN:VEVENT");
    check(ics.contains("UID:" + uid), "generated ICS carries the UID");
    check(!ics.contains("\\n\nDругий"), "DESCRIPTION newline was escaped, not left raw");

    const QString wrapped = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + ics.mid(ics.indexOf("BEGIN:VEVENT"));
    Q_UNUSED(wrapped);

    const QList<EventRow> parsed = IcalParser::parseVEvents(ics, "test-calendar", "/cal/event1.ics");
    check(parsed.size() == 1, "parsed exactly one VEVENT back out");
    if (parsed.size() == 1) {
        const EventRow &row = parsed.first();
        check(row.uid == uid, "UID round-tripped");
        check(row.summary == summary, "SUMMARY round-tripped (incl. comma/semicolon/backslash/quote)");
        check(row.location == location, "LOCATION round-tripped (incl. apostrophe/guillemets)");
        check(row.description == description, "DESCRIPTION round-tripped (incl. embedded newline)");
        check(row.calendarId == "test-calendar", "calendarId was stamped on as passed in");
        check(row.href == "/cal/event1.ics", "href was stamped on as passed in");
        check(!row.allDay, "timed event was not misdetected as all-day");
        check(row.startsAtUtc.startsWith("2026-07-10T10:00:00"), "DTSTART parsed back to the right instant");
    }

    // All-day event: "YYYYMMDD" form, no time component.
    const QString allDayIcs = QStringLiteral("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\n"
                                              "UID:allday-1\r\nDTSTART:20260101\r\nSUMMARY:New Year\r\n"
                                              "END:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> allDayParsed = IcalParser::parseVEvents(allDayIcs, "cal", "/cal/allday.ics");
    check(allDayParsed.size() == 1 && allDayParsed.first().allDay, "8-digit DTSTART detected as all-day");

    // Folded line per RFC 5545 (continuation starts with a space).
    const QString foldedIcs = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:fold-1\r\n"
                                              "DTSTART:20260101T100000Z\r\n"
                                              "SUMMARY:A very long summary that has been\r\n folded onto a second line\r\n"
                                              "END:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> foldedParsed = IcalParser::parseVEvents(foldedIcs, "cal", "/cal/folded.ics");
    check(foldedParsed.size() == 1
              && foldedParsed.first().summary == "A very long summary that has beenfolded onto a second line",
          "RFC 5545 line unfolding joins continuation lines correctly");

    // Malformed VEVENT (no UID) must be skipped, not crash or half-parse.
    const QString malformed = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nDTSTART:20260101T100000Z\r\n"
                                              "SUMMARY:No UID here\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    check(IcalParser::parseVEvents(malformed, "cal", "/cal/malformed.ics").isEmpty(), "VEVENT missing UID is skipped, not crashed on");

    // Time zones. This is the real bug from live use: iCloud showed
    // "Стоматолог" at 14:00, Lowacal showed 17:00 (Kyiv is UTC+3 in July).
    const QString kyivIcs = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:tz-1\r\n"
                                           "DTSTART;TZID=Europe/Kiev:20260708T140000\r\n"
                                           "DTEND;TZID=Europe/Kiev:20260708T150000\r\n"
                                           "SUMMARY:Стоматолог\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> kyiv = IcalParser::parseVEvents(kyivIcs, "cal", "/cal/tz1.ics");
    check(kyiv.size() == 1 && kyiv.first().startsAtUtc.startsWith("2026-07-08T11:00:00"),
          "14:00 Europe/Kiev in July is 11:00 UTC (summer time, UTC+3), not 14:00 UTC");
    check(kyiv.size() == 1 && kyiv.first().endsAtUtc.startsWith("2026-07-08T12:00:00"), "DTEND honours TZID too");

    const QString winterIcs = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:tz-2\r\n"
                                             "DTSTART;TZID=\"Europe/Kyiv\":20261216T140000\r\n"
                                             "SUMMARY:Winter\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> winter = IcalParser::parseVEvents(winterIcs, "cal", "/cal/tz2.ics");
    check(winter.size() == 1 && winter.first().startsAtUtc.startsWith("2026-12-16T12:00:00"),
          "same zone in December is UTC+2 (and the new Europe/Kyiv spelling, quoted, works)");

    const QString nyIcs = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:tz-3\r\n"
                                         "DTSTART;TZID=America/New_York:20260708T090000\r\n"
                                         "SUMMARY:Call\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> ny = IcalParser::parseVEvents(nyIcs, "cal", "/cal/tz3.ics");
    check(ny.size() == 1 && ny.first().startsAtUtc.startsWith("2026-07-08T13:00:00"),
          "an event in another zone (09:00 New York) converts correctly");

    const QString floatingIcs = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:tz-4\r\n"
                                               "DTSTART:20260708T140000\r\n"
                                               "SUMMARY:Floating\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> floating = IcalParser::parseVEvents(floatingIcs, "cal", "/cal/tz4.ics");
    const QString expectedFloating =
        QDateTime(QDate(2026, 7, 8), QTime(14, 0), QTimeZone::systemTimeZone()).toUTC().toString(Qt::ISODate);
    check(floating.size() == 1 && floating.first().startsAtUtc == expectedFloating,
          "a time with no zone is local wall-clock time, not UTC");

    // Birthday-style yearly recurrence: DTSTART stuck in some old year
    // should get shifted to the current year, or it'd never show up
    // browsing a present-day calendar (this is what "Дні народження"
    // syncing but never appearing turned out to be).
    const QString birthdayIcs = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:birthday-1\r\n"
                                                "DTSTART;VALUE=DATE:20180715\r\nDTEND;VALUE=DATE:20180716\r\n"
                                                "RRULE:FREQ=YEARLY\r\nSUMMARY:День народження\r\n"
                                                "END:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> birthdayParsed = IcalParser::parseVEvents(birthdayIcs, "cal", "/cal/birthday.ics");
    check(birthdayParsed.size() == 1, "yearly-recurring VEVENT still parses to exactly one row");
    if (birthdayParsed.size() == 1) {
        const int thisYear = QDate::currentDate().year();
        check(birthdayParsed.first().startsAtUtc.startsWith(QString::number(thisYear) + "-07-15"),
              "FREQ=YEARLY event's stored year was shifted to the current year, month/day preserved");
    }

    // A non-yearly recurring event (e.g. FREQ=WEEKLY) should NOT get its
    // year shifted - only the birthday/anniversary case is handled.
    const QString weeklyIcs = QStringLiteral("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:weekly-1\r\n"
                                              "DTSTART:20200101T100000Z\r\nRRULE:FREQ=WEEKLY\r\n"
                                              "SUMMARY:Weekly sync\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    const QList<EventRow> weeklyParsed = IcalParser::parseVEvents(weeklyIcs, "cal", "/cal/weekly.ics");
    check(weeklyParsed.size() == 1 && weeklyParsed.first().startsAtUtc.startsWith("2020-01-01"),
          "FREQ=WEEKLY is left alone (year not shifted) - only YEARLY gets the birthday treatment");
}

void testDatabase()
{
    std::printf("-- Database --\n");

    // Regression guard for the ~/.local/share/lowacal/lowacal/cache.db
    // double-nesting bug: defaultPath() must not contain the org/app
    // name twice in a row.
    const QString path = Database::defaultPath();
    check(!path.contains("lowacal/lowacal"), "defaultPath() is not doubly-nested (the org/app name bug)");
    check(path.endsWith("/lowacal/cache.db"), "defaultPath() ends with the expected, single /lowacal/cache.db");

    Database db("test_connection");
    const QString tmpPath = QDir::temp().filePath(QStringLiteral("lowacal_test_%1.db").arg(QUuid::createUuid().toString(QUuid::Id128)));
    check(db.open(tmpPath), "database opens and applies schema");

    check(db.upsertAccount("acc1", "icloud", "Test iCloud", "https://caldav.icloud.com", "test@example.com"),
          "upsertAccount succeeds");
    check(db.upsertCalendar("acc1:/cal/", "acc1", "/cal/", "Work", "#3DAEE9", "ctag-1"), "upsertCalendar succeeds");

    const QList<CalendarRow> calendars = db.listCalendars();
    check(calendars.size() == 1, "listCalendars returns the one calendar we inserted");
    if (!calendars.isEmpty()) {
        check(calendars.first().accountLabel == "Test iCloud", "listCalendars joins the account label correctly");
        check(calendars.first().visible, "new calendar defaults to visible");
    }

    check(db.calendarCtag("acc1:/cal/") == "ctag-1", "calendarCtag round-trips");
    check(db.setCalendarVisible("acc1:/cal/", false), "setCalendarVisible succeeds");
    check(!db.listCalendars().first().visible, "visibility change is reflected on next read");

    EventRow event;
    event.uid = "evt-1";
    event.href = "/cal/evt-1.ics";
    event.calendarId = "acc1:/cal/";
    event.summary = "Стендап";
    event.startsAtUtc = "2026-07-10T09:00:00Z";
    event.endsAtUtc = "2026-07-10T09:15:00Z";
    check(db.upsertEvent(event), "upsertEvent succeeds");

    db.setCalendarVisible("acc1:/cal/", true);
    const QList<EventRow> inRange = db.listEventsInRange("2026-07-01T00:00:00Z", "2026-07-31T00:00:00Z");
    check(inRange.size() == 1 && inRange.first().summary == "Стендап",
          "listEventsInRange finds the event and preserves Unicode text");
    check(inRange.first().color == "#3DAEE9", "listEventsInRange joins the calendar colour correctly");

    const QList<EventRow> outOfRange = db.listEventsInRange("2027-01-01T00:00:00Z", "2027-01-31T00:00:00Z");
    check(outOfRange.isEmpty(), "listEventsInRange correctly excludes events outside the window");

    // Reproduces the exact real-world bug found via a live iCloud sync:
    // distinct events (different href - the server-guaranteed unique
    // resource identifier) that happen to share the same iCalendar UID
    // (a data-quality artifact, not something this app can control - see
    // README). Before switching the key from uid to href, the second
    // INSERT hit the PRIMARY KEY constraint and rolled back the *entire*
    // calendar's sync; after that fix but before this one, it silently
    // overwrote the first event instead of storing both. Now, keyed by
    // href, both are correctly stored as the distinct events they are.
    EventRow dup1;
    dup1.uid = "shared-uid";
    dup1.href = "/cal/res-a.ics";
    dup1.calendarId = "acc1:/cal/";
    dup1.summary = "День народження Мольфара";
    dup1.startsAtUtc = "2026-07-01T10:00:00Z";
    dup1.endsAtUtc = "2026-07-01T11:00:00Z";
    EventRow dup2;
    dup2.uid = "shared-uid"; // same UID as dup1 - the real-world data quality issue
    dup2.href = "/cal/res-b.ics"; // but a different href - a genuinely different resource
    dup2.calendarId = "acc1:/cal/";
    dup2.summary = "День народження Лося";
    dup2.startsAtUtc = "2026-07-08T14:00:00Z";
    dup2.endsAtUtc = "2026-07-08T15:00:00Z";
    EventRow normal;
    normal.uid = "normal-1";
    normal.href = "/cal/res-c.ics";
    normal.calendarId = "acc1:/cal/";
    normal.summary = "Ordinary event";
    normal.startsAtUtc = "2026-07-10T09:00:00Z";
    normal.endsAtUtc = "2026-07-10T09:30:00Z";

    const bool replaceOk = db.replaceEventsForCalendar("acc1:/cal/", {dup1, dup2, normal});
    check(replaceOk, "replaceEventsForCalendar does not fail/rollback on a shared UID within the batch");
    const QList<EventRow> afterReplace = db.listEventsInRange("2026-07-01T00:00:00Z", "2026-07-31T00:00:00Z");
    check(afterReplace.size() == 3,
          "all 3 distinct-href events are stored despite two sharing a UID - the actual RA Guide/"
          "Дні народження bug, now fixed by keying on href instead of uid");

    // Same href appearing twice (e.g. re-synced after an edit) should
    // still correctly collapse to one row - href *is* meant to be a
    // real key, this isn't a free-for-all.
    EventRow edited = dup1;
    edited.summary = "День народження Мольфара (перенесено)";
    const bool replaceOk2 = db.replaceEventsForCalendar("acc1:/cal/", {dup1, edited});
    check(replaceOk2, "re-inserting the same href twice does not fail");
    const QList<EventRow> afterEdit = db.listEventsInRange("2026-07-01T00:00:00Z", "2026-07-31T00:00:00Z");
    check(afterEdit.size() == 1 && afterEdit.first().summary == edited.summary,
          "same href correctly collapses to one row (last write wins) - href is a real key, not ignored");

    QFile::remove(tmpPath);
}

void testCalendarColorParsing()
{
    std::printf("-- CalDAV response parsing (real captured iCloud structure) --\n");

    // Trimmed from an actual PROPFIND response dumped via LOWACAL_DEBUG_DUMP=1
    // against real iCloud - two propstat blocks per response, the real
    // color in the 200 OK block, an empty placeholder in a 404 block for
    // the namespace iCloud doesn't support. This is exactly the shape
    // that was silently producing #3DAEE9 for every single calendar.
    const QByteArray xml = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<multistatus xmlns="DAV:">
  <response xmlns="DAV:">
    <href>/8294990837/calendars/725E6161-07ED-43D8-AFF6-4E1ECD800D93/</href>
    <propstat>
      <prop>
        <resourcetype xmlns="DAV:"><collection/><calendar xmlns="urn:ietf:params:xml:ns:caldav"/></resourcetype>
        <displayname xmlns="DAV:">RA Guide</displayname>
        <getctag xmlns="http://calendarserver.org/ns/">abc123</getctag>
        <calendar-color xmlns="http://apple.com/ns/ical/">#FF9500FF</calendar-color>
      </prop>
      <status>HTTP/1.1 200 OK</status>
    </propstat>
    <propstat>
      <prop>
        <calendar-color xmlns="http://calendarserver.org/ns/"/>
      </prop>
      <status>HTTP/1.1 404 Not Found</status>
    </propstat>
  </response>
</multistatus>)";

    const QList<CalDavClient::DiscoveredCalendar> calendars = CalDavClient::parseCalendarList(xml);
    check(calendars.size() == 1, "one calendar parsed from the real captured response shape");
    if (!calendars.isEmpty()) {
        check(calendars.first().displayName == "RA Guide", "display name parsed correctly");
        check(calendars.first().color == "#FF9500",
              "real color from the 200 OK propstat wins - not clobbered by the empty 404 one");
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv); // needed for QSqlDatabase's event-loop-free but Qt-object-requiring internals
    // Same as main.cpp - matters for testDatabase()'s defaultPath() check,
    // which is specifically guarding against a bug that only manifested
    // because both of these were set to the same value.
    QCoreApplication::setApplicationName("lowacal");
    QCoreApplication::setOrganizationName("lowacal");

    testIcalRoundTrip();
    testDatabase();
    testCalendarColorParsing();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
