#include "icalparser.h"

#include <QDebug>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>
#include <QUuid>

namespace {

QString unescapeText(QString value)
{
    // Order matters: backslash last, so we don't double-unescape.
    value.replace("\\n", "\n");
    value.replace("\\N", "\n");
    value.replace("\\,", ",");
    value.replace("\\;", ";");
    value.replace("\\\\", "\\");
    return value;
}

QString escapeText(QString value)
{
    value.replace("\\", "\\\\");
    value.replace(",", "\\,");
    value.replace(";", "\\;");
    value.replace("\n", "\\n");
    return value;
}

// RFC 5545 line unfolding: a line that starts with a space or tab is a
// continuation of the previous line (with that one leading whitespace
// character removed). Servers fold long lines this way; without
// unfolding, a SUMMARY or DESCRIPTION that happens to wrap mid-value
// would silently parse as garbage.
QStringList unfoldLines(const QString &icsText)
{
    const QStringList rawLines = icsText.split(QRegularExpression("\r\n|\n|\r"));
    QStringList out;
    for (const QString &line : rawLines) {
        if (!line.isEmpty() && (line.at(0) == ' ' || line.at(0) == '\t') && !out.isEmpty()) {
            out.last() += line.mid(1);
        } else {
            out.append(line);
        }
    }
    return out;
}

// Splits "NAME;PARAM=VAL:VALUE" into just the bare property name,
// ignoring parameters - all we currently need DTSTART/DTEND's implicit
// "is this all-day" signal, which we derive from value length instead
// (see parseIcalDateTime), so parameters aren't parsed out separately.
QString propertyName(const QString &line)
{
    const int colon = line.indexOf(':');
    if (colon < 0)
        return QString();
    const QString head = line.left(colon);
    const int semi = head.indexOf(';');
    return semi >= 0 ? head.left(semi) : head;
}

QString propertyValue(const QString &line)
{
    const int colon = line.indexOf(':');
    return colon >= 0 ? line.mid(colon + 1) : QString();
}

// Parses DTSTART/DTEND/DTSTAMP-style values: "YYYYMMDD" (all-day),
// "YYYYMMDDTHHMMSSZ" (UTC), or "YYYYMMDDTHHMMSS" (floating/local time -
// treated as UTC, since resolving it properly means reading the TZID
// parameter or a VTIMEZONE block, which this parser doesn't do yet; see
// the header comment and README for the honest caveat on this).
// Value of one parameter from "NAME;PARAM=VAL;...:VALUE" (e.g. TZID), or
// an empty string. Quotes around the value are dropped.
QString propertyParam(const QString &line, const QString &param)
{
    const int colon = line.indexOf(':');
    if (colon < 0)
        return QString();
    const QStringList parts = line.left(colon).split(';');
    for (int i = 1; i < parts.size(); ++i) {
        const int eq = parts.at(i).indexOf('=');
        if (eq > 0 && parts.at(i).left(eq).compare(param, Qt::CaseInsensitive) == 0) {
            QString v = parts.at(i).mid(eq + 1);
            if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"'))
                v = v.mid(1, v.size() - 2);
            return v;
        }
    }
    return QString();
}

// The zone an event's wall-clock time is written in. Calendar apps write
// "14:00 in Europe/Kyiv" (DTSTART;TZID=Europe/Kyiv:...T140000), not UTC.
// No TZID at all means "floating": the same wall-clock time wherever you
// are, i.e. the system zone.
QTimeZone zoneFor(const QString &tzid)
{
    if (tzid.isEmpty())
        return QTimeZone::systemTimeZone();

    QStringList candidates{tzid};
    // Some producers prefix the IANA name ("/freeassociation.../Europe/Kyiv").
    const QStringList segments = tzid.split('/', Qt::SkipEmptyParts);
    if (segments.size() > 2)
        candidates << segments.at(segments.size() - 2) + '/' + segments.last();
    // Outlook/Exchange write Windows zone names ("FLE Standard Time").
    const QByteArray fromWindows = QTimeZone::windowsIdToDefaultIanaId(tzid.toUtf8());
    if (!fromWindows.isEmpty())
        candidates << QString::fromUtf8(fromWindows);
    // The zone was renamed in tzdata; a system may ship only one spelling.
    if (tzid.endsWith(QLatin1String("Europe/Kiev")))
        candidates << QStringLiteral("Europe/Kyiv");
    if (tzid.endsWith(QLatin1String("Europe/Kyiv")))
        candidates << QStringLiteral("Europe/Kiev");

    for (const QString &candidate : candidates) {
        const QTimeZone zone(candidate.toUtf8());
        if (zone.isValid())
            return zone;
    }
    qDebug() << "IcalParser: unknown time zone" << tzid << "- treating the time as local";
    return QTimeZone::systemTimeZone();
}

// Parses DTSTART/DTEND values into UTC:
//   "YYYYMMDD"          all-day (kept as UTC midnight of that date)
//   "YYYYMMDDTHHMMSSZ"  already UTC
//   "YYYYMMDDTHHMMSS"   wall-clock time in `tzid` (or floating if empty)
// The last form used to be read as if it were UTC, which showed every
// event created on a phone shifted by the local UTC offset (+2/+3 h in
// Kyiv) and fired its reminders that much too late.
QDateTime parseIcalDateTime(const QString &value, const QString &tzid)
{
    if (value.size() == 8) {
        const QDate date = QDate::fromString(value, "yyyyMMdd");
        return QDateTime(date, QTime(0, 0), QTimeZone::utc());
    }
    if (value.endsWith('Z')) {
        const QDateTime dt = QDateTime::fromString(value.chopped(1), "yyyyMMddTHHmmss");
        return QDateTime(dt.date(), dt.time(), QTimeZone::utc());
    }
    const QDateTime dt = QDateTime::fromString(value, "yyyyMMddTHHmmss");
    return QDateTime(dt.date(), dt.time(), zoneFor(tzid)).toUTC();
}

// Not full RRULE expansion (see the header comment for why that's out of
// scope) - just enough to stop the single most common case of "recurring
// event is invisible" from happening: birthdays and anniversaries are
// FREQ=YEARLY events whose stored DTSTART is whatever year the event was
// first created (often years ago), so without this they'd never appear
// in any *current* month view at all, only in whatever old year they
// started in. Shifting to the current year isn't correct for every
// possible use of yearly recurrence, but it's exactly right for the
// birthday/anniversary case, which is the overwhelming majority of
// FREQ=YEARLY events in practice.
QDateTime shiftToCurrentYear(const QDateTime &original)
{
    const int currentYear = QDate::currentDate().year();
    const QDate d = original.date();
    const QDate shifted(currentYear, d.month(), d.day());
    if (!shifted.isValid()) {
        // Feb 29 falling in a non-leap current year, most likely -
        // return the untouched original rather than an invalid date.
        return original;
    }
    return QDateTime(shifted, original.time(), original.timeZone());
}

bool isYearlyRecurrence(const QString &rrule)
{
    return rrule.contains("FREQ=YEARLY", Qt::CaseInsensitive);
}

} // namespace

QList<EventRow> IcalParser::parseVEvents(const QString &icsText, const QString &calendarId, const QString &href)
{
    QList<EventRow> out;
    const QStringList lines = unfoldLines(icsText);

    bool inEvent = false;
    QString uid, summary, description, location, rrule;
    QDateTime dtstart, dtend;
    bool allDay = false;
    bool haveUid = false, haveStart = false;

    for (const QString &line : lines) {
        if (line == "BEGIN:VEVENT") {
            inEvent = true;
            uid.clear();
            summary.clear();
            description.clear();
            location.clear();
            rrule.clear();
            dtstart = QDateTime();
            dtend = QDateTime();
            allDay = false;
            haveUid = false;
            haveStart = false;
            continue;
        }
        if (line == "END:VEVENT") {
            if (inEvent && haveUid && haveStart) {
                EventRow row;
                row.uid = uid;
                row.href = href;
                row.calendarId = calendarId;
                row.summary = summary;
                row.description = description;
                row.location = location;
                QDateTime start = dtstart;
                QDateTime end = dtend.isValid() ? dtend : dtstart;
                if (isYearlyRecurrence(rrule)) {
                    start = shiftToCurrentYear(start);
                    end = shiftToCurrentYear(end);
                }
                row.startsAtUtc = start.toString(Qt::ISODate);
                row.endsAtUtc = end.toString(Qt::ISODate);
                row.allDay = allDay;
                out.append(row);
            } else if (inEvent) {
                qDebug() << "IcalParser: skipping VEVENT missing UID or DTSTART";
            }
            inEvent = false;
            continue;
        }
        if (!inEvent)
            continue;

        const QString name = propertyName(line);
        const QString value = propertyValue(line);
        if (name == "UID") {
            uid = value;
            haveUid = !uid.isEmpty();
        } else if (name == "SUMMARY") {
            summary = unescapeText(value);
        } else if (name == "DESCRIPTION") {
            description = unescapeText(value);
        } else if (name == "LOCATION") {
            location = unescapeText(value);
        } else if (name == "RRULE") {
            rrule = value;
        } else if (name == "DTSTART") {
            allDay = value.size() == 8;
            dtstart = parseIcalDateTime(value, propertyParam(line, QStringLiteral("TZID")));
            haveStart = dtstart.isValid();
        } else if (name == "DTEND") {
            dtend = parseIcalDateTime(value, propertyParam(line, QStringLiteral("TZID")));
        }
    }

    return out;
}

QString IcalParser::buildVEventIcs(const QString &uid, const QString &summary, const QDateTime &startsUtc,
                                    const QDateTime &endsUtc, const QString &location, const QString &description)
{
    const QString fmt = "yyyyMMddTHHmmss'Z'";
    QStringList lines;
    lines << "BEGIN:VCALENDAR";
    lines << "VERSION:2.0";
    lines << "PRODID:-//lowacal//lowacal//EN";
    lines << "BEGIN:VEVENT";
    lines << ("UID:" + uid);
    lines << ("DTSTAMP:" + QDateTime::currentDateTimeUtc().toString(fmt));
    lines << ("DTSTART:" + startsUtc.toUTC().toString(fmt));
    lines << ("DTEND:" + endsUtc.toUTC().toString(fmt));
    lines << ("SUMMARY:" + escapeText(summary));
    if (!location.isEmpty())
        lines << ("LOCATION:" + escapeText(location));
    if (!description.isEmpty())
        lines << ("DESCRIPTION:" + escapeText(description));
    lines << "END:VEVENT";
    lines << "END:VCALENDAR";

    // RFC 5545 wants CRLF line endings; most servers tolerate bare LF
    // too, but this costs nothing to get right.
    return lines.join("\r\n") + "\r\n";
}
