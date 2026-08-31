import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import HyprFM
import Quill as Q

Q.Dialog {
    id: root
    anchors.fill: parent
    z: 1000
    dialogWidth: 430
    title: "Archive password"
    subtitle: root.fileName
    initialFocusItem: passwordField

    property string filePath: ""
    property string fileName: {
        var parts = String(root.filePath).split("/")
        return parts[parts.length - 1] || root.filePath
    }
    property string errorText: ""

    signal confirmed(string password)

    function openFor(path, retry) {
        root.filePath = path
        root.errorText = retry ? "That password did not work. Try again." : ""
        passwordField.text = ""
        root.open()
    }

    function submit() {
        root.errorText = ""
        var pass = passwordField.text
        if (pass === "") {
            root.errorText = "Enter the archive password."
            return
        }
        root.confirmed(pass)
        root.accept()
    }

    onOpened: Qt.callLater(function() { passwordField.inputItem.forceActiveFocus() })

    Q.TextField {
        id: passwordField
        Layout.fillWidth: true
        variant: "filled"
        placeholder: "Password"
        echoMode: TextInput.Password
        inputItem.Keys.onReturnPressed: root.submit()
        onTextChanged: root.errorText = ""
    }

    Text {
        Layout.fillWidth: true
        visible: root.errorText !== ""
        text: root.errorText
        color: Theme.error
        font.pointSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 12

        Q.Button {
            text: "Cancel"
            variant: "ghost"
            size: "small"
            onClicked: root.reject()
        }

        Q.Button {
            text: "Unlock"
            variant: "primary"
            size: "small"
            onClicked: root.submit()
        }
    }
}
