import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    width: 900
    height: 600
    visible: true
    title: "CodeLeveling"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: 12
            Label { text: "XP: " + App.totalXp; Layout.leftMargin: 12 }
            Label { text: "Level: " + App.level }
            Item { Layout.fillWidth: true }
            Button { text: "Refresh"; onClicked: App.refresh() }
            Button { text: "Dailies"; onClicked: nav.push(dailyPage) }
            Button { text: "Leaderboard"; onClicked: nav.push(leaderboardPage) }
            ComboBox {
                id: userBox
                model: App.users
                Layout.preferredWidth: 180

                // set current selection whenever model or currentUser changes
                Component.onCompleted: sync()
                onModelChanged: sync()

                function sync() {
                    for (var i = 0; i < count; ++i) {
                        if (textAt(i) === App.currentUser) {
                            currentIndex = i
                            return
                        }
                    }
                    currentIndex = -1
                }

                Connections {
                    target: App
                    function onCurrentUserChanged() { userBox.sync() }
                    function onUsersChanged() { userBox.sync() }
                }

                onActivated: App.setCurrentUser(currentText)
            }

            TextField {
                id: newUserField
                placeholderText: "New user..."
                Layout.preferredWidth: 160
            }

            Button {
                text: "Add/Switch"
                onClicked: {
                    if (newUserField.text.trim().length > 0) {
                        App.setCurrentUser(newUserField.text.trim())
                        newUserField.text = ""
                    }
                }
            }
        }
    }

    Popup {
        id: snack
        x: (parent.width - width) / 2
        y: parent.height - height - 24
        width: Math.min(parent.width * 0.9, 420)
        height: implicitHeight
        modal: false
        focus: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property string text: ""

        background: Rectangle {
            radius: 10
            opacity: 0.92
            border.width: 1
        }

        contentItem: Item {
            implicitWidth: 420
            implicitHeight: msg.implicitHeight + 24

            Label {
                id: msg
                anchors.fill: parent
                anchors.margins: 12
                text: snack.text
                wrapMode: Text.Wrap
            }
        }


        Timer {
            id: snackTimer
            interval: 1800
            onTriggered: snack.close()
        }

        function show(msg) {
            snack.text = msg
            snack.open()
            snackTimer.restart()
        }
    }


    Connections {
        target: App
        function onToast(msg) { snack.show(msg) }
    }

    StackView {
        id: nav
        anchors.fill: parent
        anchors.margins: 16
        initialItem: questListPage
    }

    Component {
        id: questListPage

        ListView {
            spacing: 10
            model: App.quests

            delegate: Rectangle {
                width: ListView.view.width
                height: 72
                radius: 12
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: modelData.title; font.pixelSize: 16 }
                        Label { text: "Topic: " + modelData.topic + " | Difficulty: " + modelData.difficulty; opacity: 0.7 }
                    }

                    Label { text: modelData.status; opacity: 0.8 }

                    Button {
                        text: "Open"
                        enabled: modelData.status !== "locked"
                        onClicked: nav.push(questViewPage, { quest: modelData })
                    }
                }
            }
        }
    }

    Component {
        id: questViewPage

        Item {
            property var quest
            property var currentQ: ({})
            property int selectedIndex: -1

            function loadQuestion() {
                selectedIndex = -1
                currentQ = App.getNextQuestion(quest.id)
                if (!currentQ || currentQ.id === undefined) {
                    snack.show("Quest mastered! ✅")
                }
            }

            Component.onCompleted: loadQuestion()

            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Button { text: "Back"; onClicked: nav.pop() }
                    Label { text: quest.title; font.pixelSize: 18 }
                    Item { Layout.fillWidth: true }
                    Label { text: quest.status; opacity: 0.8 }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 12
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 12

                        // ====== QUIZ UI ======
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 12
                            visible: currentQ && currentQ.id !== undefined

                            Text {
                                text: App.getLesson(quest.id)
                                textFormat: Text.MarkdownText
                                wrapMode: Text.Wrap
                                opacity: 0.85
                            }


                            Label {
                                text: currentQ.prompt !== undefined ? currentQ.prompt : ""
                                wrapMode: Text.Wrap
                                font.pixelSize: 16
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Repeater {
                                    model: currentQ.choices !== undefined ? currentQ.choices : []

                                    delegate: RadioButton {
                                        Layout.fillWidth: true
                                        text: modelData
                                        checked: index === selectedIndex
                                        onClicked: selectedIndex = index
                                    }
                                }
                            }

                            Item { Layout.fillHeight: true }

                            RowLayout {
                                Layout.fillWidth: true

                                Button { text: "Next"; onClicked: loadQuestion() }

                                Item { Layout.fillWidth: true }

                                Button {
                                    text: "Submit"
                                    enabled: currentQ.id !== undefined && selectedIndex !== -1
                                    onClicked: {
                                        var ok = App.submitAnswer(currentQ.id, selectedIndex)
                                        if (ok) loadQuestion()
                                    }
                                }
                            }
                        }

                        // ====== MASTERED UI ======
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 12
                            visible: !currentQ || currentQ.id === undefined

                            Item { Layout.fillHeight: true }

                            Label {
                                text: "Quest mastered ✅"
                                font.pixelSize: 18
                                horizontalAlignment: Text.AlignHCenter
                                Layout.fillWidth: true
                            }

                            Label {
                                text: "You answered all questions correctly."
                                opacity: 0.7
                                horizontalAlignment: Text.AlignHCenter
                                Layout.fillWidth: true
                            }

                            Item { Layout.fillHeight: true }

                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 12

                                Button { text: "Back to quests"; onClicked: nav.pop() }
                                Button { text: "Refresh quests"; onClicked: App.refresh() }
                            }
                        }
                    }
                }
            }
        }
    }
    Component {
        id: dailyPage
        Item {
            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Button { text: "Back"; onClicked: nav.pop() }
                    Label { text: "Daily Tasks"; font.pixelSize: 18 }
                    Item { Layout.fillWidth: true }
                    Button { text: "Refresh"; onClicked: App.refreshDaily() }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 10
                    model: App.dailyTasks

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 64
                        radius: 12
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            Label { text: modelData.title; Layout.fillWidth: true }
                            Label { text: "+" + modelData.xp + " XP"; opacity: 0.7 }
                            Button {
                                text: modelData.done ? "Done" : "Complete"
                                enabled: !modelData.done
                                onClicked: App.completeDailyTask(modelData.id)
                            }
                        }
                    }
                }
            }
        }
    }
    Component {
        id: leaderboardPage
        Item {
            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Button { text: "Back"; onClicked: nav.pop() }
                    Label { text: "Leaderboard"; font.pixelSize: 18 }
                    Item { Layout.fillWidth: true }
                    Button { text: "Refresh"; onClicked: App.refreshLeaderboard() }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8
                    model: App.leaderboard

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 56
                        radius: 10
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            Label { text: (index+1) + "."; width: 36 }
                            Label { text: modelData.username; Layout.fillWidth: true }
                            Label { text: "XP " + modelData.xp; opacity: 0.7 }
                            Label { text: "Lv " + modelData.level; opacity: 0.7 }
                            Label { text: "Score " + Math.round(modelData.score); opacity: 0.7 }
                        }
                    }
                }
            }
        }
    }

}
