#pragma once

class CalendarBridge;
class SyncWorker;

// Every bridge <-> worker signal connection, in one place, so the real
// app (main.cpp) and the end-to-end UI test use the *same* wiring rather
// than a test-only copy that could silently drift from it.
void registerAppMetaTypes();

// Picks the UI language - the user's choice from Settings ("ui/language":
// "" = follow the system, "en", "uk"), else the system locale - makes it
// the default QLocale (so dates, month and day names in QML follow the
// same language as the text), and installs the matching translations:
// the app's own (:/i18n/lowacal_<lang>.qm) and Qt's (standard dialog
// buttons etc.). Call after the QCoreApplication exists, before any QML
// is loaded. English is the source language, so it needs no .qm at all.
void setupLanguage();
void wireBridgeAndWorker(CalendarBridge *bridge, SyncWorker *worker);
