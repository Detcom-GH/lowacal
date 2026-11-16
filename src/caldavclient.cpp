#include "caldavclient.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>
#include <QDebug>

namespace {

// Opt-in raw-response dumping: `LOWACAL_DEBUG_DUMP=1 ./lowacal` writes
// every WebDAV response body to a timestamped file under
// /tmp/lowacal-debug/, so real server responses (exact UIDs, exact
// calendar-color XML) can be inspected directly instead of inferred
// from parsed/derived data. Off by default - these can contain personal
// calendar data.
void maybeDumpResponse(const QString &label, const QByteArray &body)
{
    static const bool enabled = QProcessEnvironment::systemEnvironment().value("LOWACAL_DEBUG_DUMP") == "1";
    if (!enabled)
        return;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/lowacal-debug";
    QDir().mkpath(dir);
    const QString filename = QStringLiteral("%1/%2_%3.xml")
                                  .arg(dir, QDateTime::currentDateTime().toString("hhmmss.zzz"), label);
    QFile f(filename);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(body);
        qInfo().noquote() << "  [debug dump]" << filename;
    }
}

} // namespace

CalDavClient::CalDavClient(QString username, QString password, QObject *parent)
    : QObject(parent), m_username(std::move(username)), m_password(std::move(password))
{
}

QByteArray CalDavClient::authHeader() const
{
    if (!m_bearerToken.isEmpty())
        return "Bearer " + m_bearerToken.toUtf8();
    return "Basic " + (m_username + ":" + m_password).toUtf8().toBase64();
}

QString CalDavClient::joinUrl(const QString &base, const QString &href)
{
    // Proper RFC 3986 resolution: an absolute-path href ("/dav/x/")
    // REPLACES base's path rather than being appended to it. The earlier
    // string-concatenation version only worked because iCloud's base URL
    // has no path; against e.g. https://host/remote.php/dav (Nextcloud) it
    // produced /remote.php/dav/remote.php/dav/... and every request 404'd.
    return QUrl(base).resolved(QUrl(href)).toString(QUrl::FullyEncoded);
}

QByteArray CalDavClient::webdavRequest(const QString &method, const QString &url, const QString &depth,
                                        const QByteArray &body, bool *ok, const QString &debugLabel)
{
    // Redirects are followed by hand rather than left to Qt's automatic
    // policy: what Qt does with a custom verb + request body on a
    // 301/302 isn't something this code should have to depend on, and
    // CalDAV discovery leans on redirects (/.well-known/caldav -> the
    // real DAV root). Each hop re-sends the same method and body.
    QString currentUrl = url;
    m_lastStatus = 0;
    for (int hop = 0; hop < 5; ++hop) {
        QNetworkRequest req{QUrl(currentUrl)};
        req.setRawHeader("Depth", depth.toUtf8());
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
        req.setRawHeader("Authorization", authHeader());
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        req.setTransferTimeout(20000); // a dead host fails in 20s instead of hanging the add-account dialog

        auto *bodyBuffer = new QBuffer;
        bodyBuffer->setData(body);
        bodyBuffer->open(QIODevice::ReadOnly);

        QNetworkReply *reply = m_nam.sendCustomRequest(req, method.toUtf8(), bodyBuffer);
        bodyBuffer->setParent(reply);

        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray responseBody = reply->readAll();
        const QString netErrorString = reply->errorString();
        // Raw header, resolved by hand below: Qt's parsed LocationHeader is
        // empty for a *relative* Location ("/remote.php/dav/"), which RFC
        // 7231 allows and real servers send - caught by the end-to-end
        // test, where the .well-known redirect was silently not followed.
        const QUrl location = QUrl::fromEncoded(reply->rawHeader("Location"));
        reply->deleteLater();

        m_lastStatus = status;

        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
            && location.isValid()) {
            currentUrl = QUrl(currentUrl).resolved(location).toString(QUrl::FullyEncoded);
            continue;
        }

        if (!debugLabel.isEmpty())
            maybeDumpResponse(debugLabel, responseBody);

        m_lastFinalUrl = currentUrl;

        if (status == 0) {
            m_lastError = QStringLiteral("network error requesting %1: %2").arg(currentUrl, netErrorString);
            qWarning().noquote() << "CalDavClient:" << m_lastError;
            if (ok)
                *ok = false;
            return QByteArray();
        }
        if (status < 200 || (status >= 300 && status != 207)) {
            m_lastError = QStringLiteral("CalDAV server returned %1 for %2").arg(status).arg(currentUrl);
            qWarning().noquote() << "CalDavClient:" << m_lastError << "-" << responseBody.left(300);
            if (ok)
                *ok = false;
            return responseBody;
        }
        if (ok)
            *ok = true;
        return responseBody;
    }

    m_lastError = QStringLiteral("too many redirects starting from %1").arg(url);
    if (ok)
        *ok = false;
    return QByteArray();
}

QString CalDavClient::discoverEndpoint(const QString &userUrl, QString *principalHref, bool *ok)
{
    // What people type is rarely the exact DAV root: "cloud.example.com",
    // "https://cloud.example.com/", the Nextcloud web UI URL... Try the
    // typed URL first, then the standard well-known locations, and keep
    // the first one that answers a current-user-principal PROPFIND. An
    // auth failure stops the search immediately - the credentials are
    // wrong, and trying more URLs would just repeat the same 401.
    QString base = userUrl.trimmed();
    if (!base.contains(QStringLiteral("://")))
        base.prepend(QStringLiteral("https://"));
    while (base.endsWith('/'))
        base.chop(1);

    const QUrl parsed(base);
    const QString origin = parsed.scheme() + QStringLiteral("://") + parsed.authority();

    QStringList candidates{
        base,
        base + QStringLiteral("/.well-known/caldav"),
        origin + QStringLiteral("/.well-known/caldav"),
        base + QStringLiteral("/remote.php/dav/"), // Nextcloud / ownCloud
        base + QStringLiteral("/dav.php/"), // Baikal
    };
    candidates.removeDuplicates();

    for (const QString &candidate : candidates) {
        bool found = false;
        const QString principal = discoverPrincipal(candidate, &found);
        if (found) {
            if (principalHref)
                *principalHref = principal;
            if (ok)
                *ok = true;
            return m_lastFinalUrl; // where the redirects actually landed
        }
        if (m_lastStatus == 401 || m_lastStatus == 403)
            break;
    }
    if (ok)
        *ok = false;
    return QString();
}

QString CalDavClient::discoverPrincipal(const QString &baseUrl, bool *ok)
{
    static const QByteArray body = R"(<?xml version="1.0" encoding="utf-8"?>
<D:propfind xmlns:D="DAV:">
  <D:prop><D:current-user-principal/></D:prop>
</D:propfind>)";

    bool requestOk = false;
    const QByteArray xml = webdavRequest("PROPFIND", baseUrl, "0", body, &requestOk, "principal");
    if (!requestOk) {
        if (ok)
            *ok = false;
        return QString();
    }
    const QString href = extractFirstHref(xml, "current-user-principal");
    if (href.isEmpty()) {
        m_lastError = "server did not return current-user-principal";
        if (ok)
            *ok = false;
        return QString();
    }
    if (ok)
        *ok = true;
    return href;
}

QString CalDavClient::discoverCalendarHome(const QString &baseUrl, const QString &principalHref, bool *ok)
{
    static const QByteArray body = R"(<?xml version="1.0" encoding="utf-8"?>
<D:propfind xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav">
  <D:prop><C:calendar-home-set/></D:prop>
</D:propfind>)";

    const QString url = joinUrl(baseUrl, principalHref);
    bool requestOk = false;
    const QByteArray xml = webdavRequest("PROPFIND", url, "0", body, &requestOk, "homeset");
    if (!requestOk) {
        if (ok)
            *ok = false;
        return QString();
    }
    const QString href = extractFirstHref(xml, "calendar-home-set");
    if (href.isEmpty()) {
        m_lastError = "principal did not return a calendar-home-set";
        if (ok)
            *ok = false;
        return QString();
    }
    if (ok)
        *ok = true;
    return href;
}

QList<CalDavClient::DiscoveredCalendar> CalDavClient::listCalendars(const QString &baseUrl, const QString &homeHref,
                                                                     bool *ok)
{
    static const QByteArray body = R"(<?xml version="1.0" encoding="utf-8"?>
<D:propfind xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav" xmlns:CS="http://calendarserver.org/ns/" xmlns:A="http://apple.com/ns/ical/">
  <D:prop>
    <D:resourcetype/>
    <D:displayname/>
    <CS:getctag/>
    <CS:calendar-color/>
    <A:calendar-color/>
  </D:prop>
</D:propfind>)";

    const QString url = joinUrl(baseUrl, homeHref);
    bool requestOk = false;
    const QByteArray xml = webdavRequest("PROPFIND", url, "1", body, &requestOk, "calendarlist");
    if (!requestOk) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    return parseCalendarList(xml);
}

QList<QPair<QString, QString>> CalDavClient::fetchEvents(const QString &baseUrl, const QString &calendarHref,
                                                          const QString &fromUtc, const QString &toUtc, bool *ok)
{
    const QByteArray body = QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<C:calendar-query xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav">
  <D:prop>
    <D:getetag/>
    <C:calendar-data/>
  </D:prop>
  <C:filter>
    <C:comp-filter name="VCALENDAR">
      <C:comp-filter name="VEVENT">
        <C:time-range start="%1" end="%2"/>
      </C:comp-filter>
    </C:comp-filter>
  </C:filter>
</C:calendar-query>)")
                                  .arg(fromUtc, toUtc)
                                  .toUtf8();

    const QString url = joinUrl(baseUrl, calendarHref);
    bool requestOk = false;
    QString label = "events_" + calendarHref;
    label.replace(QRegularExpression("[^A-Za-z0-9]+"), "_");
    const QByteArray xml = webdavRequest("REPORT", url, "1", body, &requestOk, label);
    if (!requestOk) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    return extractCalendarData(xml);
}

QString CalDavClient::putEvent(const QString &baseUrl, const QString &calendarHref, const QString &uid,
                                const QString &icsBody)
{
    QString hrefDir = calendarHref;
    if (!hrefDir.endsWith('/'))
        hrefDir += '/';
    const QString eventHref = hrefDir + uid + ".ics";
    const QString url = joinUrl(baseUrl, eventHref);

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "text/calendar; charset=utf-8");
    req.setRawHeader("Authorization", authHeader());
    req.setRawHeader("If-None-Match", "*");

    QNetworkReply *reply = m_nam.put(req, icsBody.toUtf8());
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    reply->deleteLater();

    if (status < 200 || status >= 300) {
        m_lastError = QStringLiteral("CalDAV server rejected new event (%1): %2").arg(status).arg(
            QString::fromUtf8(responseBody.left(300)));
        qWarning().noquote() << "CalDavClient:" << m_lastError;
        return QString();
    }
    return eventHref;
}

bool CalDavClient::updateEvent(const QString &baseUrl, const QString &eventHref, const QString &icsBody)
{
    const QString url = joinUrl(baseUrl, eventHref);

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "text/calendar; charset=utf-8");
    req.setRawHeader("Authorization", authHeader());

    QNetworkReply *reply = m_nam.put(req, icsBody.toUtf8());
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    reply->deleteLater();

    if (status < 200 || status >= 300) {
        m_lastError = QStringLiteral("CalDAV server rejected event update (%1): %2").arg(status).arg(
            QString::fromUtf8(responseBody.left(300)));
        qWarning().noquote() << "CalDavClient:" << m_lastError;
        return false;
    }
    return true;
}

bool CalDavClient::deleteEvent(const QString &baseUrl, const QString &eventHref)
{
    const QString url = joinUrl(baseUrl, eventHref);

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Authorization", authHeader());

    QNetworkReply *reply = m_nam.deleteResource(req);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    reply->deleteLater();

    // 404 counts as success for a delete - the goal state ("this event
    // doesn't exist on the server") is already true.
    if ((status < 200 || status >= 300) && status != 404) {
        m_lastError = QStringLiteral("CalDAV server rejected event deletion (%1): %2").arg(status).arg(
            QString::fromUtf8(responseBody.left(300)));
        qWarning().noquote() << "CalDavClient:" << m_lastError;
        return false;
    }
    return true;
}

QString CalDavClient::extractFirstHref(const QByteArray &xml, const QString &within)
{
    QXmlStreamReader reader(xml);
    bool inTarget = false;
    while (!reader.atEnd() && !reader.hasError()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name.compare(within, Qt::CaseInsensitive) == 0) {
                inTarget = true;
            } else if (inTarget && name.compare("href", Qt::CaseInsensitive) == 0) {
                const QString text = reader.readElementText().trimmed();
                if (!text.isEmpty())
                    return text;
            }
        } else if (reader.isEndElement()) {
            if (reader.name().compare(within, Qt::CaseInsensitive) == 0)
                inTarget = false;
        }
    }
    return QString();
}

QList<CalDavClient::DiscoveredCalendar> CalDavClient::parseCalendarList(const QByteArray &xml)
{
    QList<DiscoveredCalendar> results;
    QXmlStreamReader reader(xml);

    QString curHref, curName, curColor, curCtag;
    bool isCalendar = false;

    while (!reader.atEnd() && !reader.hasError()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name.compare("response", Qt::CaseInsensitive) == 0) {
                curHref.clear();
                curName.clear();
                curColor.clear();
                curCtag.clear();
                isCalendar = false;
            } else if (name.compare("href", Qt::CaseInsensitive) == 0) {
                curHref = reader.readElementText().trimmed();
            } else if (name.compare("displayname", Qt::CaseInsensitive) == 0) {
                curName = reader.readElementText().trimmed();
            } else if (name.compare("getctag", Qt::CaseInsensitive) == 0) {
                curCtag = reader.readElementText().trimmed();
            } else if (name.compare("calendar-color", Qt::CaseInsensitive) == 0) {
                QString c = reader.readElementText().trimmed();
                if (c.size() == 9 && c.startsWith('#'))
                    c = c.left(7); // Apple's #RRGGBBAA -> #RRGGBB
                // Confirmed via a real response dump: Apple sends calendar-color
                // TWICE per calendar - the real value under a 200 OK propstat,
                // and an empty placeholder under a 404 propstat for whichever
                // namespace (CS: vs A:) it doesn't support. Document order has
                // the real one first, so an unconditional overwrite here was
                // clobbering it with the empty one every time.
                if (!c.isEmpty())
                    curColor = c;
            } else if (name.compare("calendar", Qt::CaseInsensitive) == 0) {
                isCalendar = true;
            }
        } else if (reader.isEndElement()) {
            const QString name = reader.name().toString();
            if (name.compare("response", Qt::CaseInsensitive) == 0 && isCalendar && !curHref.isEmpty()) {
                DiscoveredCalendar cal;
                cal.href = curHref;
                cal.displayName = curName.isEmpty() ? tr("Untitled") : curName;
                cal.color = curColor.isEmpty() ? QStringLiteral("#3DAEE9") : curColor;
                cal.ctag = curCtag;
                results.append(cal);
            }
        }
    }
    return results;
}

QList<QPair<QString, QString>> CalDavClient::extractCalendarData(const QByteArray &xml)
{
    QList<QPair<QString, QString>> results;
    QXmlStreamReader reader(xml);
    QString curHref, curData;

    while (!reader.atEnd() && !reader.hasError()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name.compare("response", Qt::CaseInsensitive) == 0) {
                curHref.clear();
                curData.clear();
            } else if (name.compare("href", Qt::CaseInsensitive) == 0 && curHref.isEmpty()) {
                curHref = reader.readElementText().trimmed();
            } else if (name.compare("calendar-data", Qt::CaseInsensitive) == 0) {
                curData = reader.readElementText();
            }
        } else if (reader.isEndElement()) {
            const QString name = reader.name().toString();
            if (name.compare("response", Qt::CaseInsensitive) == 0 && !curHref.isEmpty()
                && !curData.trimmed().isEmpty()) {
                results.append({curHref, curData});
            }
        }
    }
    return results;
}
