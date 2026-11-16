#pragma once

#include <QList>
#include <QString>

// Account configuration.
//
// v1 deliberately keeps this simple: accounts live in a small JSON file
// (~/.config/lowacal/accounts.json, see accounts.example.json at the repo
// root), and only the password is kept out of it - looked up from the
// system keyring via the `secret-tool` CLI (part of libsecret, already a
// dependency of a stock Plasma session's Secret Service / KWallet
// integration). Shelling out to a CLI rather than linking a keyring
// library (e.g. QtKeychain) is a deliberate choice for this rewrite:
// fewer external dependencies to version-mismatch on, which was the
// entire point of moving off cxx-qt.
//
// A proper "add account" wizard inside the QML UI is the natural next
// feature to build; for now this file is what you edit by hand.

#include "account.h" // AccountConfig lives there now

namespace Config {

QString configPath();

// Reads accounts.json and, for each account, looks up its password via
// `secret-tool lookup service lowacal username <username>`. An account
// whose keyring lookup fails is skipped (logged, not fatal) rather than
// aborting the whole load - one bad account shouldn't take down sync for
// every other configured account.
QList<AccountConfig> loadAccounts();

// LEGACY, migration only. Before QtKeychain, passwords were stored with
// `secret-tool store ... service lowacal username <user>`. CalendarBridge
// uses this once per account to move such a password into the keychain;
// returns empty if secret-tool is absent or has no entry.
QString legacyLookupPassword(const QString &username);

} // namespace Config
