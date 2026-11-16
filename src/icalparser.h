#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include "database.h"

// Turns raw iCalendar (RFC 5545) text - as returned inside a CalDAV
// calendar-data element - into EventRows, and builds new VEVENT bodies
// for uploading. Hand-rolled rather than pulling in a separate iCalendar
// library: the read side only needs a handful of properties (UID,
// SUMMARY, DTSTART, DTEND, LOCATION, DESCRIPTION) and RFC 5545's line
// structure is simple enough that a small, well-tested parser is less
// risk than an unfamiliar third-party dependency.
//
// Deliberately out of scope: general RRULE expansion (weekly/monthly
// recurrence, exceptions via RECURRENCE-ID, etc.) - a recurring event
// still only stores its first-parsed occurrence. One narrow, high-value
// exception: FREQ=YEARLY events (the overwhelming majority of which are
// birthdays/anniversaries in practice) get their stored date shifted to
// the current year, so they actually show up when browsing the calendar
// instead of being invisible outside whatever year they were created in.
// See icalparser.cpp's shiftToCurrentYear for the honest caveats on that.
namespace IcalParser {

// Parses every VEVENT found in `icsText` (a full VCALENDAR blob, as
// CalDAV servers return them) into EventRows. `calendarId` and `href`
// are stamped onto each row since the raw ICS text has no notion of
// which local calendar or server resource it came from - the caller
// (SyncWorker) knows that from context. `href`, not the VEVENT's own
// UID, is what actually identifies the row uniquely in storage - see
// database.h for why.
QList<EventRow> parseVEvents(const QString &icsText, const QString &calendarId, const QString &href);

// Builds a minimal, RFC 5545-valid VCALENDAR/VEVENT body ready to PUT to
// a CalDAV server.
QString buildVEventIcs(const QString &uid, const QString &summary, const QDateTime &startsUtc,
                        const QDateTime &endsUtc, const QString &location, const QString &description);

} // namespace IcalParser
