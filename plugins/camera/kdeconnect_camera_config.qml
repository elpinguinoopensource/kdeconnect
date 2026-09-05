/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kdeconnect

Kirigami.ScrollablePage {
    id: root

    property string device

    KdeConnectPluginConfig {
        id: config
        deviceId: root.device
        pluginName: "kdeconnect_camera"
    }

    Kirigami.FormLayout {
        QQC2.SpinBox {
            Kirigami.FormData.label: i18nc("@label:spinbox stream width in pixels", "Width:")
            Layout.fillWidth: true
            from: 160
            to: 7680
            stepSize: 16
            editable: true
            value: config.getInt("width", 1280)
            onValueModified: config.set("width", value)
        }

        QQC2.SpinBox {
            Kirigami.FormData.label: i18nc("@label:spinbox stream height in pixels", "Height:")
            Layout.fillWidth: true
            from: 120
            to: 4320
            stepSize: 16
            editable: true
            value: config.getInt("height", 720)
            onValueModified: config.set("height", value)
        }

        QQC2.SpinBox {
            Kirigami.FormData.label: i18nc("@label:spinbox frame rate", "Frame rate:")
            Layout.fillWidth: true
            from: 1
            to: 240
            editable: true
            value: config.getInt("fps", 30)
            onValueModified: config.set("fps", value)
            //: Suffix for the frame rate spin box (frames per second)
            suffix: " " + i18ndc("kdeconnect-plugins", "Abbreviation for frames per second", "fps")
        }

        QQC2.SpinBox {
            Kirigami.FormData.label: i18nc("@label:spinbox", "Bitrate:")
            Layout.fillWidth: true
            from: 250
            to: 50000
            stepSize: 250
            editable: true
            value: config.getInt("bitrate", 4000)
            onValueModified: config.set("bitrate", value)
            //: Suffix for the bitrate spin box (kilobits per second)
            suffix: " " + i18ndc("kdeconnect-plugins", "Abbreviation for kilobits per second", "kbps")
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18nc("@label:checkbox", "Cover image:")
            text: i18nc("@label:checkbox", "Show KDE Connect logo when idle")
            checked: config.getBool("showCover", true)
            onToggled: config.set("showCover", checked)
        }
    }
}
