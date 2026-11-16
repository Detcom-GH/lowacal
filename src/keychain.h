#pragma once

#include <QObject>
#include <QString>
#include <functional>

// Per-account passwords in the system keychain (KWallet on Plasma, GNOME
// Keyring / any Secret Service provider elsewhere) via QtKeychain.
//
// Replaces the earlier `secret-tool` CLI shell-out, which (a) needed the
// user to run a terminal command by hand, and (b) cannot work inside a
// Flatpak sandbox at all, where that binary doesn't exist. QtKeychain
// talks to the keychain over D-Bus, which Flatpak permits.
//
// Used from the GUI thread only: QtKeychain is designed around the main
// event loop, and that's where every KDE app drives it from. Callbacks
// fire asynchronously on that same thread; `context` scopes them, so a
// callback never runs after its context object is destroyed.
namespace Keychain {

void readPassword(const QString &accountId, QObject *context,
                  std::function<void(bool ok, const QString &password, const QString &error)> callback);

void writePassword(const QString &accountId, const QString &password, QObject *context,
                   std::function<void(bool ok, const QString &error)> callback);

// Fire-and-forget; a missing entry is not an error.
void deletePassword(const QString &accountId);

} // namespace Keychain
