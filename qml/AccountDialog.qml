import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// Two steps: pick a provider, then fill in a short form. "Підключити"
// verifies the credentials against the real server (CalendarBridge.addAccount)
// before anything is saved, so a wrong password is reported right here
// instead of turning into a silently broken account.
Controls.Popup {
    id: dialog
    // Width from the window overlay, NOT `parent`: these dialogs are declared
    // inside the 236 px sidebar, so parent.width squeezed them to a strip
    // (found by rendering the real UI).
    width: Math.min(440, Controls.Overlay.overlay ? Controls.Overlay.overlay.width - 40 : 440)
    modal: true
    focus: true
    closePolicy: (busy && !waitingForBrowser) ? Controls.Popup.NoAutoClose : (Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside)
    anchors.centerIn: Controls.Overlay.overlay

    signal succeeded(string message)

    property string provider: "" // "" = picker page, "icloud", "google", "caldav"
    property bool busy: false
    // Google only: true while the user is signing in in their browser.
    // Unlike the server check that follows, this wait can be cancelled.
    property bool waitingForBrowser: false
    readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)

    function reset() {
        provider = ""
        busy = false
        waitingForBrowser = false
        errorLabel.text = ""
        nameField.text = ""
        urlField.text = ""
        userField.text = ""
        passField.text = ""
    }

    function choose(kind) {
        provider = kind
        errorLabel.text = ""
        if (kind !== "google")
            Qt.callLater(function() { (kind === "caldav" ? urlField : userField).forceActiveFocus() })
    }

    readonly property bool canSubmit: !busy && (provider === "google" || (
        userField.text.trim().length > 0
        && passField.text.length > 0
        && (provider !== "caldav" || urlField.text.trim().length > 0)))

    function submit() {
        if (!canSubmit) return
        errorLabel.text = ""
        busy = true
        if (provider === "google") {
            waitingForBrowser = true
            CalendarBridge.addGoogleAccount(nameField.text)
        } else {
            CalendarBridge.addAccount(provider, nameField.text, urlField.text, userField.text, passField.text)
        }
    }

    function cancelBrowserWait() {
        CalendarBridge.cancelGoogleSignIn()
        waitingForBrowser = false
        busy = false
    }

    Connections {
        target: CalendarBridge
        function onAccountAddResult(ok, message) {
            if (!dialog.busy) return // not a request from this dialog
            dialog.busy = false
            dialog.waitingForBrowser = false
            if (ok) {
                dialog.succeeded(message)
                dialog.close()
            } else {
                errorLabel.text = message
            }
        }
        function onGoogleSignedIn() { dialog.waitingForBrowser = false }
    }

    onOpened: reset()
    onClosed: {
        passField.text = "" // don't keep the password in memory longer than needed
        if (waitingForBrowser)
            cancelBrowserWait()
    }

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        radius: Kirigami.Units.cornerRadius
        border.color: dialog.hairline
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            objectName: "heading"
            level: 3
            text: dialog.provider === "icloud" ? qsTr("Connect iCloud")
                : dialog.provider === "google" ? qsTr("Connect Google Calendar")
                : dialog.provider === "caldav" ? qsTr("Connect a CalDAV server")
                : qsTr("Add calendar")
        }

        // ---------- step 1: provider picker ----------
        ColumnLayout {
            visible: dialog.provider === ""
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Repeater {
                model: [
                    { kind: "icloud", title: "iCloud", subtitle: qsTr("Apple calendars: iPhone, Mac, iPad"), icon: "office-calendar", enabled: true },
                    { kind: "google", title: qsTr("Google Calendar"), subtitle: CalendarBridge.googleAvailable ? qsTr("Gmail and Google Workspace calendars") : qsTr("Not available in this build"), icon: "im-google", enabled: CalendarBridge.googleAvailable },
                    { kind: "caldav", title: qsTr("Other CalDAV server"), subtitle: "Nextcloud, Fastmail, Baïkal, Radicale, mailbox.org…", icon: "network-server", enabled: true }
                ]
                delegate: Controls.ItemDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    enabled: modelData.enabled
                    opacity: enabled ? 1.0 : 0.5
                    onClicked: dialog.choose(modelData.kind)

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            source: modelData.icon
                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label { text: modelData.title; font.bold: true }
                            Controls.Label {
                                Layout.fillWidth: true
                                text: modelData.subtitle
                                opacity: 0.7
                                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                                wrapMode: Text.Wrap
                            }
                        }
                        Kirigami.Icon {
                            source: "go-next"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                            opacity: 0.5
                        }
                    }
                }
            }
        }

        // ---------- step 2: form ----------
        ColumnLayout {
            visible: dialog.provider !== ""
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            enabled: !dialog.busy

            // iCloud needs an app-specific password; most people have never
            // made one, so explain it and link straight to the right page.
            Controls.Label {
                visible: dialog.provider === "icloud"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                opacity: 0.8
                text: qsTr("Apple doesn't give third-party apps your main password. Create a separate app-specific password: Sign-In and Security → App-Specific Passwords → “+”. It takes a minute.")
            }
            Controls.Button {
                visible: dialog.provider === "icloud"
                text: qsTr("Create an app-specific password on Apple's website")
                icon.name: "internet-services"
                onClicked: Qt.openUrlExternally("https://appleid.apple.com/account/manage")
            }

            // Google: nothing to type here - the password goes into the
            // user's own browser, on Google's page.
            Controls.Label {
                visible: dialog.provider === "google"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("You'll sign in on Google's own page in your browser. Lowacal never sees your Google password.")
            }

            Controls.Label { visible: dialog.provider === "caldav"; text: qsTr("Server address") }
            Controls.TextField {
                id: urlField
            objectName: "urlField"
                visible: dialog.provider === "caldav"
                Layout.fillWidth: true
                placeholderText: "cloud.example.com"
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                onAccepted: userField.forceActiveFocus()
            }

            Controls.Label { visible: dialog.provider !== "google"; text: dialog.provider === "icloud" ? qsTr("Apple ID (email)") : qsTr("Username") }
            Controls.TextField {
                id: userField
            objectName: "userField"
                visible: dialog.provider !== "google"
                Layout.fillWidth: true
                placeholderText: dialog.provider === "icloud" ? "name@icloud.com" : ""
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onAccepted: passField.forceActiveFocus()
            }

            Controls.Label { visible: dialog.provider !== "google"; text: dialog.provider === "icloud" ? qsTr("App-specific password") : qsTr("Password") }
            Kirigami.PasswordField {
                id: passField
            objectName: "passField"
                visible: dialog.provider !== "google"
                Layout.fillWidth: true
                placeholderText: dialog.provider === "icloud" ? "xxxx-xxxx-xxxx-xxxx" : ""
                onAccepted: dialog.submit()
            }

            Controls.Label { text: qsTr("Display name (optional)"); opacity: 0.8 }
            Controls.TextField {
                id: nameField
            objectName: "nameField"
                Layout.fillWidth: true
                placeholderText: dialog.provider === "icloud" ? "iCloud" : dialog.provider === "google" ? "Google" : qsTr("e.g. Work Nextcloud")
                onAccepted: dialog.submit()
            }

            Controls.Label {
                visible: dialog.provider === "caldav"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                opacity: 0.6
                text: qsTr("The server address alone is enough: the path to your calendars is found automatically.")
            }
        }

        Controls.Label {
            id: errorLabel
            objectName: "errorLabel"
            visible: text.length > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Kirigami.Theme.negativeTextColor
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                objectName: "backButton"
                text: (dialog.provider === "" || dialog.waitingForBrowser) ? qsTr("Cancel") : qsTr("Back")
                enabled: !dialog.busy || dialog.waitingForBrowser
                onClicked: dialog.waitingForBrowser ? dialog.cancelBrowserWait()
                         : dialog.provider === "" ? dialog.close() : dialog.choose("")
            }
            Item { Layout.fillWidth: !dialog.busy }
            Controls.BusyIndicator {
                visible: dialog.busy
                running: visible
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
            }
            Controls.Label {
                visible: dialog.busy
                objectName: "busyLabel"
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: dialog.waitingForBrowser ? qsTr("Waiting for sign-in in your browser…") : qsTr("Checking…")
                opacity: 0.7
            }
            Controls.Button {
                visible: dialog.provider !== ""
                highlighted: true
                text: dialog.provider === "google" ? qsTr("Sign in with Google") : qsTr("Connect")
                enabled: dialog.canSubmit
                onClicked: dialog.submit()
            }
        }
    }
}
