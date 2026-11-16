#include <QApplication>
#include <QDesktopServices>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>

#include "appwiring.h"
#include "branding.h"
#include "platform.h"
#include "calendarbridge.h"
#include "syncworker.h"
#include "traycontroller.h"

int main(int argc, char *argv[])
{
    // QSystemTrayIcon (TrayController) needs QtWidgets and a proper
    // QApplication rather than QGuiApplication, even though the rest of
    // this app is plain QML/Quick - see README for how the earlier
    // Rust/cxx-qt version of this project found that out the hard way.
    // AccountConfig crosses threads in queued signals.
    registerAppMetaTypes();

    QApplication app(argc, argv);
    app.setApplicationName("lowacal");
    app.setOrganizationName("lowacal");
    // Ties the process to lowacal.desktop - matters for correct window
    // grouping in the taskbar and for notification attribution.
    app.setDesktopFileName(QStringLiteral(LOWACAL_APP_ID));
    app.setApplicationVersion(QStringLiteral(LOWACAL_VERSION));
    app.setWindowIcon(Branding::appIcon());

    // --hidden: started by launch-at-login - come up in the tray only.
    const bool startHidden = app.arguments().contains(QStringLiteral("--hidden"));

    // Already running (typically: sitting in the tray after autostart)?
    // Then bring that copy's window up and quit this one. A --hidden start
    // that finds a running copy just quits: nothing to show.
    SingleInstance instance;
    if (!instance.claim(!startHidden))
        return 0;
    // Closing the window hides it (see main.qml's onClosing) rather than
    // quitting - the tray is what keeps the process alive after that, so
    // Qt's default "quit when the last window closes" has to be off.
    app.setQuitOnLastWindowClosed(false);
    setupLanguage(); // before any QML is created, so every qsTr() binding sees it

    auto *bridge = new CalendarBridge(&app);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("CalendarBridge", bridge);
    // Google sign-in page: the user's default browser (via the portal when sandboxed).
    QObject::connect(bridge, &CalendarBridge::openUrlRequested, &app, [](const QUrl &url) { QDesktopServices::openUrl(url); });
    engine.rootContext()->setContextProperty("lowacalStartHidden", startHidden);

    // --- background sync/notification worker, on its own thread ---
    auto *workerThread = new QThread(&app);
    auto *worker = new SyncWorker;
    worker->moveToThread(workerThread);
    QObject::connect(workerThread, &QThread::started, worker, &SyncWorker::start);
    // Shutdown order matters: on aboutToQuit, first ask the worker (on
    // its own thread, via a queued connection) to stop its timers - the
    // owning thread is the only one allowed to - and only then quit the
    // thread's event loop. Queued slots on the same thread run in
    // connection order, so shutdown() is processed before quit() takes
    // effect. Without this, the timers get destroyed later from the main
    // thread ("Timers cannot be stopped from another thread").
    QObject::connect(&app, &QCoreApplication::aboutToQuit, worker, &SyncWorker::shutdown);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, workerThread, &QThread::quit);
    // NOTE: deliberately *not* using deleteLater() here connected to
    // QThread::finished. That looks right but likely never actually
    // fires: deleteLater() schedules deletion for the next time the
    // *target object's thread* processes an event loop iteration, and by
    // the time finished() is emitted, workerThread's event loop has
    // already stopped - there may be no further iteration left to run
    // it. `worker` is deleted explicitly and deterministically below,
    // once workerThread->wait() confirms the thread has actually
    // stopped, in both exit paths.

    wireBridgeAndWorker(bridge, worker);

    workerThread->start();
    // After start(): the worker's start() slot (DB open) runs before any
    // queued request reaches it, so setAccounts always finds an open DB.
    bridge->loadAccounts();

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/dev/lowa/calendar/qml/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        // QML failed to load - shut the worker thread down cleanly before
        // returning rather than letting main() end with it still running.
        // Skipping this was the actual bug behind the "QSqlDatabase
        // requires a QCoreApplication" crash: returning here used to tear
        // down QApplication (a stack object, destroyed as main() unwinds)
        // while the worker thread was still alive on a separate OS thread,
        // potentially mid-way through opening its own SQLite connection.
        // (aboutToQuit never fires on this path, so invoke shutdown -
        // queued onto the worker's own thread - explicitly here.)
        QMetaObject::invokeMethod(worker, &SyncWorker::shutdown, Qt::QueuedConnection);
        workerThread->quit();
        workerThread->wait();
        delete worker;
        return -1;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());

    // --- tray: native GUI-thread object, connects directly to the window ---
    auto *tray = new TrayController(&app);
    if (window) {
        auto showWindow = [window]() {
            window->show();
            window->raise();
            window->requestActivate();
        };
        QObject::connect(tray, &TrayController::showWindowRequested, window, showWindow);
        QObject::connect(&instance, &SingleInstance::activateRequested, window, showWindow);
    }
    QObject::connect(tray, &TrayController::syncRequested, bridge, &CalendarBridge::syncNow);
    QObject::connect(tray, &TrayController::quitRequested, &app, &QCoreApplication::quit);
    QObject::connect(worker, &SyncWorker::eventNotification, tray, &TrayController::showNotification);

    const int exitCode = app.exec();

    // Same reasoning as the early-return path above: aboutToQuit already
    // asked the worker thread to quit, but that's a request, not a
    // guarantee it's finished by the time app.exec() returns - wait()
    // blocks until it actually has, so nothing races QApplication's
    // teardown immediately after this function returns.
    workerThread->quit();
    workerThread->wait();
    delete worker;

    return exitCode;
}
