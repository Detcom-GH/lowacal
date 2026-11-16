// End-to-end test of the add-account flow, driving the REAL QML
// (Sidebar, AccountDialog, SettingsDialog) and the REAL backend wiring
// (appwiring.cpp - the same function main.cpp uses) against:
//   - tests/fake_caldav.py, a Nextcloud-shaped CalDAV server
//   - a real Secret Service keychain (gnome-keyring in a private D-Bus session)
// Run via tests/run_ui_flow_test.sh, which sets all of that up.
//
// Kirigami itself is replaced by tests/kirigami-stub (KF6 isn't available
// where this was developed); the stub mirrors only properties that exist
// in real Kirigami, so it can't paper over invented API.

#include <QApplication>
#include <QElapsedTimer>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QLocale>
#include <QFile>
#include <QSettings>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QThread>
#include <cstdio>
#include <functional>

#include "appwiring.h"
#include "calendarbridge.h"
#include "keychain.h"
#include "syncworker.h"

namespace {
int failures = 0;
QStringList qmlWarnings;

void check(bool ok, const QString &what)
{
    std::printf("  %s - %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++failures;
}

bool waitUntil(const std::function<bool()> &cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

int jsLength(QObject *o, const char *prop)
{
    const QVariant v = o->property(prop);
    if (v.metaType() == QMetaType::fromType<QJSValue>())
        return v.value<QJSValue>().property("length").toInt();
    return v.toList().size();
}

QJSValue jsProp(QObject *o, const char *prop)
{
    const QVariant v = o->property(prop);
    return v.metaType() == QMetaType::fromType<QJSValue>() ? v.value<QJSValue>() : QJSValue();
}

void call(QObject *o, const char *fn, const QVariant &arg = QVariant())
{
    if (arg.isValid())
        QMetaObject::invokeMethod(o, fn, Q_ARG(QVariant, arg));
    else
        QMetaObject::invokeMethod(o, fn);
}
} // namespace

int main(int argc, char *argv[])
{
    registerAppMetaTypes();
    QApplication app(argc, argv);
    app.setApplicationName("lowacal");
    app.setOrganizationName("lowacal");

    // Optional: simulate the user having picked a language in Settings.
    const QString forcedLanguage = qEnvironmentVariable("LOWACAL_TEST_UI_LANGUAGE");
    if (!forcedLanguage.isEmpty())
        QSettings().setValue("ui/language", forcedLanguage);
    setupLanguage(); // same function main.cpp uses

    // Every expected message below depends on which language actually got
    // loaded - which is exactly what this run is meant to prove.
    const bool uk = QLocale().language() == QLocale::Ukrainian;
    auto expect = [uk](const char *en, const char *ukText) { return uk ? QString::fromUtf8(ukText) : QString::fromUtf8(en); };
    std::printf("-- language: %s (system locale %s, setting \"%s\") --\n", uk ? "Ukrainian" : "English",
                qPrintable(QLocale::system().name()), qPrintable(forcedLanguage));

    const QString serverUrl = qEnvironmentVariable("FAKE_CALDAV_URL");

    auto *bridge = new CalendarBridge(&app);
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError> &list) {
        for (const QQmlError &e : list)
            qmlWarnings << e.toString();
    });
    engine.rootContext()->setContextProperty("CalendarBridge", bridge);
    engine.addImportPath(QStringLiteral(LOWACAL_TEST_DIR "/kirigami-stub"));

    auto *workerThread = new QThread(&app);
    auto *worker = new SyncWorker;
    worker->moveToThread(workerThread);
    QObject::connect(workerThread, &QThread::started, worker, &SyncWorker::start);
    wireBridgeAndWorker(bridge, worker);
    workerThread->start();
    bridge->loadAccounts();

    engine.load(QUrl::fromLocalFile(QStringLiteral(LOWACAL_TEST_DIR "/ui_flow_test.qml")));
    check(!engine.rootObjects().isEmpty(), "QML loads (Sidebar + AccountDialog + SettingsDialog)");
    if (engine.rootObjects().isEmpty()) {
        for (const QString &w : qmlWarnings)
            std::printf("    %s\n", qPrintable(w));
        return 1;
    }
    QObject *root = engine.rootObjects().first();
    QCoreApplication::processEvents();

    QObject *sidebar = root->findChild<QObject *>("sidebar");
    QObject *dialog = root->findChild<QObject *>("accountDialog");
    QObject *settings = root->findChild<QObject *>("settingsDialog");
    check(sidebar && dialog && settings, "sidebar and both dialogs found");
    if (!sidebar || !dialog || !settings)
        return 1;
    auto field = [&](const char *name) { return dialog->findChild<QObject *>(name); };

    std::printf("-- empty state --\n");
    check(jsLength(sidebar, "accounts") == 0, "fresh start: no accounts");

    QString headingText;
    qreal dialogWidth = 0;
    auto attempt = [&](const QString &url, const QString &user, const QString &pass) {
        call(dialog, "open");
        QCoreApplication::processEvents();
        call(dialog, "choose", QStringLiteral("caldav"));
        dialogWidth = dialog->property("width").toReal();
        headingText = dialog->findChild<QObject *>("heading")->property("text").toString();
        field("urlField")->setProperty("text", url);
        field("userField")->setProperty("text", user);
        field("passField")->setProperty("text", pass);
        call(dialog, "submit");
        const bool started = dialog->property("busy").toBool();
        const bool finished = waitUntil([&] { return !dialog->property("busy").toBool(); }, 40000);
        return started && finished;
    };

    std::printf("-- wrong password --\n");
    check(attempt(serverUrl, "alice@example.com", "wrong"), "submit runs and finishes");
    const QString wrongPassMsg = field("errorLabel")->property("text").toString();
    std::printf("    message: %s\n", qPrintable(wrongPassMsg));
    check(wrongPassMsg.contains(expect("Wrong username", "Невірний логін")), "wrong password -> clear auth error shown in dialog");
    std::printf("    dialog heading: %s\n", qPrintable(headingText));
    check(headingText == expect("Connect a CalDAV server", "Підключити CalDAV-сервер"), "QML text is in the selected language");
    std::printf("    add-account dialog width: %.0f\n", dialogWidth);
    check(dialogWidth >= 400, "add-account dialog is window-sized, not squeezed to the sidebar");
    check(dialog->property("visible").toBool(), "dialog stays open so the user can fix it");
    check(jsLength(sidebar, "accounts") == 0, "nothing saved on failure");

    std::printf("-- unreachable server --\n");
    check(attempt(QStringLiteral("http://127.0.0.1:9"), "alice@example.com", "secret"), "submit runs and finishes");
    const QString netMsg = field("errorLabel")->property("text").toString();
    std::printf("    message: %s\n", qPrintable(netMsg));
    check(netMsg.contains(expect("connect to the server", "з'єднатися")), "dead server -> connection error shown");

    std::printf("-- correct credentials, bare server URL (discovery via .well-known) --\n");
    check(attempt(serverUrl, "alice@example.com", "secret"), "submit runs and finishes");
    const QString okErr = field("errorLabel")->property("text").toString();
    if (!okErr.isEmpty())
        std::printf("    unexpected error: %s\n", qPrintable(okErr));
    check(okErr.isEmpty(), "no error");
    check(!dialog->property("visible").toBool(), "dialog closes on success");
    check(waitUntil([&] { return jsLength(sidebar, "accounts") == 1 && jsLength(sidebar, "calendars") == 1; }, 20000),
          "sidebar shows the new account and its calendar");
    const QString successText = sidebar->property("successText").toString();
    std::printf("    success message: %s\n", qPrintable(successText));
    check(successText.startsWith(expect("Connected", "Підключено")), "success message in the selected language");

    const QJSValue cal = jsProp(sidebar, "calendars").property(0);
    check(cal.property("displayName").toString() == QStringLiteral("Особистий"), "calendar name from server");
    check(cal.property("color").toString() == QStringLiteral("#E9643A"), "calendar color from server");
    const QString accountId = jsProp(sidebar, "accounts").property(0).property("id").toString();

    check(waitUntil([&] { return !bridge->property("syncing").toBool(); }, 20000), "first sync finishes");
    const QString events = bridge->eventsJson("2026-07-01T00:00:00Z", "2026-08-01T00:00:00Z");
    check(events.contains(QStringLiteral("Подія з фейкового Nextcloud")), "event synced through the resolved endpoint");

    std::printf("-- keychain --\n");
    QString stored;
    bool readDone = false, readOk = false;
    Keychain::readPassword(accountId, &app, [&](bool ok, const QString &pw, const QString &) {
        readOk = ok;
        stored = pw;
        readDone = true;
    });
    waitUntil([&] { return readDone; }, 10000);
    check(readOk && stored == QStringLiteral("secret"), "password stored in the system keychain (not the DB)");

    // "Restart": re-run the startup path, which must get the password back
    // from the keychain and sync without errors.
    int setCalls = 0, idleSignals = 0;
    QObject::connect(bridge, &CalendarBridge::requestSetAccounts, &app, [&] { ++setCalls; });
    QObject::connect(bridge, &CalendarBridge::syncStatusChanged, &app, [&] { if (!bridge->syncing()) ++idleSignals; });
    bridge->loadAccounts();
    // Wait for the whole chain: keychain read -> accounts handed to the
    // worker -> that sync finished. (An earlier version only slept, and
    // could move on while the keychain read was still in flight.)
    check(waitUntil([&] { return setCalls == 1; }, 20000), "startup path gets the password back from the keychain");
    idleSignals = 0;
    check(waitUntil([&] { return idleSignals > 0; }, 20000) && bridge->lastError().isEmpty(), "...and syncs cleanly with it");

    // An account removed while its password is still being read at
    // startup (e.g. KWallet waiting to be unlocked) must not come back.
    {
        // Simulate the race directly: hand the worker a list that contains an account its database doesn't have.
        AccountConfig ghost;
        ghost.id = "acc-ghost";
        ghost.kind = "caldav";
        ghost.label = "Ghost";
        ghost.baseUrl = serverUrl + "/remote.php/dav/";
        ghost.username = "alice@example.com";
        ghost.password = "secret";
        emit bridge->requestSetAccounts({ghost});
        idleSignals = 0;
        waitUntil([&] { return idleSignals > 0; }, 20000);
        QThread::msleep(100);
        QCoreApplication::processEvents();
        check(!bridge->accountsJson().contains("acc-ghost"), "a stale account list can't resurrect a removed account");
    }
    // Put the real list back for the rest of the test.
    setCalls = 0;
    bridge->loadAccounts();
    waitUntil([&] { return setCalls >= 1; }, 20000);
    idleSignals = 0;
    waitUntil([&] { return idleSignals > 0; }, 20000);

    std::printf("-- settings --\n");
    call(settings, "open");
    QCoreApplication::processEvents();
    settings->findChild<QObject *>("weekStartCombo")->setProperty("currentIndex", 1);
    call(settings, "save");
    check(bridge->firstDayOfWeek() == 1, "first day of week = Monday saved");
    settings->findChild<QObject *>("languageCombo")->setProperty("currentIndex", 2);
    call(settings, "open");
    QCoreApplication::processEvents();
    settings->findChild<QObject *>("languageCombo")->setProperty("currentIndex", 2);
    call(settings, "save");
    check(bridge->language() == QStringLiteral("uk"), "language choice saved");

    std::printf("-- branding --\n");
    QObject *about = root->findChild<QObject *>("aboutDialog");
    call(settings, "open");
    QCoreApplication::processEvents();
    QMetaObject::invokeMethod(settings->findChild<QObject *>("aboutButton"), "clicked");
    check(waitUntil([&] { return about && about->property("visible").toBool(); }, 3000),
          "Settings -> About opens the About dialog");
    check(!settings->property("visible").toBool(), "...and closes Settings behind it");
    std::printf("    about dialog width: %.0f\n", about->property("width").toReal());
    check(about->property("width").toReal() >= 340, "About dialog is window-sized, not squeezed to the sidebar");
    // The art must reach the screen exactly as drawn in data/dachshund.txt -
    // a lost backslash or quote from escaping would quietly mangle it.
    QFile artFile(QStringLiteral(LOWACAL_TEST_DIR "/../data/dachshund.txt"));
    artFile.open(QIODevice::ReadOnly);
    const QString art = QString::fromUtf8(artFile.readAll()).chopped(1); // file ends with a newline
    int pictures = 0, dachshunds = 0;
    for (QObject *o : root->findChildren<QObject *>()) {
        if (o->property("source").toString().startsWith("qrc:/icons/"))
            ++pictures;
        if (o->objectName() == "asciiDachshund" && o->property("text").toString() == art)
            ++dachshunds;
    }
    std::printf("    ascii dachshunds: %d, pictures in the UI: %d\n", dachshunds, pictures);
    check(dachshunds == 2, "the dachshund from data/dachshund.txt, character for character, in the header and in About");
    check(pictures == 0, "no picture logo inside the app window");
    call(about, "close");

    std::printf("-- remove account --\n");
    bridge->removeAccount(accountId);
    check(waitUntil([&] { return jsLength(sidebar, "accounts") == 0 && jsLength(sidebar, "calendars") == 0; }, 10000),
          "account and its calendars disappear from the sidebar");
    readDone = false;
    Keychain::readPassword(accountId, &app, [&](bool ok, const QString &, const QString &) {
        readOk = ok;
        readDone = true;
    });
    waitUntil([&] { return readDone; }, 10000);
    check(!readOk, "password removed from the keychain");

    // ------------------------------------------------------------------
    // Google: the real sign-in code against tests/fake_google.py. The only
    // thing replaced is the human: instead of a browser window, the test
    // fetches the consent URL itself and follows the redirect back to the
    // app's loopback port, exactly as a browser would.
    std::printf("-- Google --\n");
    const QString google = qEnvironmentVariable("FAKE_GOOGLE_URL");
    QNetworkAccessManager http;
    auto request = [&](const QString &method, const QString &path) {
        QNetworkReply *r = http.sendCustomRequest(QNetworkRequest(QUrl(google + path)), method.toLatin1());
        waitUntil([&] { return r->isFinished(); }, 5000);
        const QByteArray body = r->readAll();
        r->deleteLater();
        return body;
    };
    auto stat = [&](const char *key) { return QJsonDocument::fromJson(request("GET", "/test/stats")).object().value(key).toInt(); };

    bool playBrowser = true;
    QUrl consentUrl;
    QObject::connect(bridge, &CalendarBridge::openUrlRequested, &app, [&](const QUrl &url) {
        consentUrl = url;
        if (playBrowser)
            QObject::connect(http.get(QNetworkRequest(url)), &QNetworkReply::finished, &app, [] {});
    });
    auto googleAttempt = [&] {
        call(dialog, "open");
        QCoreApplication::processEvents();
        call(dialog, "choose", QStringLiteral("google"));
        call(dialog, "submit");
        return dialog->property("busy").toBool();
    };
    auto waitIdle = [&] { return waitUntil([&] { return !dialog->property("busy").toBool(); }, 30000); };

    check(bridge->googleAvailable(), "Google is offered when the build has an OAuth client");

    playBrowser = false; // the user never finishes in the browser...
    check(googleAttempt() && dialog->property("waitingForBrowser").toBool(), "sign-in opens the browser and waits");
    const QString q = consentUrl.query(QUrl::FullyDecoded);
    check(q.contains("code_challenge_method=S256") && q.contains("redirect_uri=http://127.0.0.1:")
              && q.contains("https://www.googleapis.com/auth/calendar"),
          "consent URL asks for the calendar scope, with PKCE and a loopback redirect");
    call(dialog, "cancelBrowserWait"); // ...and presses Cancel
    check(!dialog->property("busy").toBool() && dialog->property("visible").toBool(), "waiting for the browser can be cancelled");
    playBrowser = true;

    request("POST", "/test/mode?m=deny"); // user clicks "Cancel" on Google's page
    check(googleAttempt() && waitIdle(), "attempt runs and finishes");
    const QString denyMsg = field("errorLabel")->property("text").toString();
    std::printf("    message: %s\n", qPrintable(denyMsg));
    check(denyMsg.contains(expect("was cancelled", "скасовано")), "declining on Google's page -> clear message, nothing saved");

    request("POST", "/test/mode?m=no_calendar"); // user unticks the calendar permission
    check(googleAttempt() && waitIdle(), "attempt runs and finishes");
    const QString scopeMsg = field("errorLabel")->property("text").toString();
    std::printf("    message: %s\n", qPrintable(scopeMsg));
    check(scopeMsg.contains(expect("wasn't granted", "не надано")), "unticked calendar permission is explained");
    check(jsLength(sidebar, "accounts") == 0, "still nothing saved");

    request("POST", "/test/mode?m=allow");
    const int pkceBefore = stat("pkce_ok");
    check(googleAttempt() && waitIdle(), "attempt runs and finishes");
    const QString googleErr = field("errorLabel")->property("text").toString();
    if (!googleErr.isEmpty())
        std::printf("    unexpected error: %s\n", qPrintable(googleErr));
    check(googleErr.isEmpty() && !dialog->property("visible").toBool(), "successful sign-in closes the dialog");
    check(waitUntil([&] { return jsLength(sidebar, "accounts") == 1 && jsLength(sidebar, "calendars") == 1; }, 20000),
          "Google account and its calendar appear in the sidebar");
    const QJSValue gAccount = jsProp(sidebar, "accounts").property(0);
    check(gAccount.property("label").toString() == "Google" && gAccount.property("username").toString() == "artem@gmail.test",
          "account is labelled Google and shows the signed-in address");
    check(jsProp(sidebar, "calendars").property(0).property("color").toString() == "#0B8043", "calendar colour from Google");
    check(stat("pkce_ok") == pkceBefore + 1 && stat("pkce_bad") == 0, "PKCE verifier accepted by the token endpoint");
    check(stat("refresh_grants") >= 1 && stat("bearer_ok") >= 3, "CalDAV requests carry an access token made from the refresh token");

    check(waitUntil([&] { return !bridge->property("syncing").toBool(); }, 20000), "first Google sync finishes");
    const QString gEvents = bridge->eventsJson("2026-07-01T00:00:00Z", "2026-08-01T00:00:00Z");
    check(gEvents.contains(QStringLiteral("Подія з Google")), "Google event synced");
    check(gEvents.contains("2026-07-20T11:00:00Z"), "14:00 Europe/Kyiv from Google is stored as 11:00 UTC (time zone honoured)");

    const QString gId = gAccount.property("id").toString();
    readDone = false;
    Keychain::readPassword(gId, &app, [&](bool ok, const QString &pw, const QString &) { readOk = ok; stored = pw; readDone = true; });
    waitUntil([&] { return readDone; }, 10000);
    check(readOk && stored == "rt-1", "only the refresh token is kept, in the system keychain");

    check(googleAttempt() && waitIdle(), "second sign-in with the same Google account");
    const QString dupMsg = field("errorLabel")->property("text").toString();
    check(dupMsg.contains(expect("already connected", "вже підключено")) && jsLength(sidebar, "accounts") == 1,
          "the same Google account can't be added twice");
    call(dialog, "close");

    request("POST", "/test/revoke"); // user removes Lowacal's access in their Google account
    bridge->syncNow();
    QThread::msleep(300);
    waitUntil([&] { return !bridge->property("syncing").toBool() && !bridge->lastError().isEmpty(); }, 20000);
    std::printf("    message: %s\n", qPrintable(bridge->lastError()));
    check(bridge->lastError().contains(expect("no longer valid", "більше не дійсний")),
          "revoked access -> the app says to sign in again instead of a vague sync error");

    bridge->removeAccount(gId);
    check(waitUntil([&] { return jsLength(sidebar, "accounts") == 0; }, 10000), "Google account removed");

    std::printf("-- QML runtime warnings --\n");
    for (const QString &w : qmlWarnings)
        std::printf("    %s\n", qPrintable(w));
    check(qmlWarnings.isEmpty(), "no QML warnings during the whole run");

    QMetaObject::invokeMethod(worker, &SyncWorker::shutdown, Qt::QueuedConnection);
    workerThread->quit();
    workerThread->wait();
    delete worker;

    std::printf("\n%s (%d failure%s)\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
