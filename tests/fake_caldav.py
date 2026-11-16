#!/usr/bin/env python3
"""Minimal Nextcloud-shaped CalDAV server for end-to-end tests.

Deliberately reproduces the things that make non-iCloud servers harder:
  - the server root is NOT a DAV endpoint (PROPFIND / -> 405)
  - /.well-known/caldav 301-redirects to the real DAV root, /remote.php/dav/
  - principal and calendar paths contain a percent-encoded '@' (%40)
  - Basic auth: alice@example.com / secret, anything else -> 401
Every request is logged to stderr so a failing test shows exactly which
URL the client asked for.
"""
import base64
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

USER, PASSWORD = "alice@example.com", "secret"
ROOT = "/remote.php/dav/"
PRINCIPAL = "/remote.php/dav/principals/users/alice%40example.com/"
HOME = "/remote.php/dav/calendars/alice%40example.com/"
CAL = HOME + "personal/"

EVENT = """BEGIN:VCALENDAR
VERSION:2.0
BEGIN:VEVENT
UID:fake-event-1
DTSTART:20260720T090000Z
DTEND:20260720T100000Z
SUMMARY:Подія з фейкового Nextcloud
END:VEVENT
END:VCALENDAR
"""


def multistatus(inner):
    return ('<?xml version="1.0" encoding="utf-8"?>'
            '<d:multistatus xmlns:d="DAV:" xmlns:cal="urn:ietf:params:xml:ns:caldav" '
            'xmlns:cs="http://calendarserver.org/ns/" xmlns:x1="http://apple.com/ns/ical/">'
            + inner + '</d:multistatus>').encode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("  [fake-caldav] %s %s\n" % (self.command, self.path))

    def _authorized(self):
        expected = "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()
        return self.headers.get("Authorization") == expected

    def _send(self, code, body=b"", headers=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(n) if n else b""

    def do_PROPFIND(self):
        self._read_body()
        path = self.path
        if path.rstrip("/") == "/.well-known/caldav":
            return self._send(301, headers={"Location": ROOT})
        if path not in (ROOT, PRINCIPAL, HOME):
            return self._send(405 if path in ("", "/") else 404)
        if not self._authorized():
            return self._send(401, headers={"WWW-Authenticate": 'Basic realm="fake"'})
        if path == ROOT:
            inner = (f'<d:response><d:href>{ROOT}</d:href><d:propstat><d:prop>'
                     f'<d:current-user-principal><d:href>{PRINCIPAL}</d:href></d:current-user-principal>'
                     '</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>')
        elif path == PRINCIPAL:
            inner = (f'<d:response><d:href>{PRINCIPAL}</d:href><d:propstat><d:prop>'
                     f'<cal:calendar-home-set><d:href>{HOME}</d:href></cal:calendar-home-set>'
                     '</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>')
        else:  # HOME, depth 1
            inner = (f'<d:response><d:href>{HOME}</d:href><d:propstat><d:prop>'
                     '<d:resourcetype><d:collection/></d:resourcetype>'
                     '</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>'
                     f'<d:response><d:href>{CAL}</d:href><d:propstat><d:prop>'
                     '<d:resourcetype><d:collection/><cal:calendar/></d:resourcetype>'
                     '<d:displayname>Особистий</d:displayname><cs:getctag>ctag-1</cs:getctag>'
                     '<x1:calendar-color>#E9643AFF</x1:calendar-color>'
                     '</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>')
        self._send(207, multistatus(inner), {"Content-Type": "application/xml; charset=utf-8"})

    def do_REPORT(self):
        self._read_body()
        if self.path != CAL:
            return self._send(404)
        if not self._authorized():
            return self._send(401)
        inner = (f'<d:response><d:href>{CAL}fake-event-1.ics</d:href><d:propstat><d:prop>'
                 f'<d:getetag>"1"</d:getetag><cal:calendar-data>{EVENT}</cal:calendar-data>'
                 '</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>')
        self._send(207, multistatus(inner), {"Content-Type": "application/xml; charset=utf-8"})

    def do_GET(self):
        self._send(200, b"<html>Nextcloud web UI</html>", {"Content-Type": "text/html"})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
