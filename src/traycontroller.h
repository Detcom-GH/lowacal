#pragma once

#include <QObject>
#include <QSystemTrayIcon>

class QMenu;

// The system tray icon. Plain QSystemTrayIcon (QtWidgets) rather than
// anything QML-facing - the whole point of this rewrite was to stop
// fighting an unfamiliar wrapper's incomplete coverage of Qt, and
// QSystemTrayIcon is about as well-trodden as Qt APIs get.
//
// Because this runs on the GUI thread (QSystemTrayIcon must), the
// "show the window" action can connect directly to the QQuickWindow in
// main.cpp - no polling flag needed the way the Rust/ksni version had to
// use to get a background thread's tray talking to the Qt thread.
class TrayController : public QObject {
    Q_OBJECT

public:
    explicit TrayController(QObject *parent = nullptr);

signals:
    void showWindowRequested();
    void syncRequested();
    void quitRequested();

public slots:
    // Called from SyncWorker (auto-queued across threads) when an event
    // is starting soon.
    void showNotification(const QString &title, const QString &body);

private:
    QSystemTrayIcon *m_tray;
    QMenu *m_menu;
};
