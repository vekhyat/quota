import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 960
    height: 820
    minimumWidth: 640
    minimumHeight: 560
    visible: true
    title: qsTr("Quota")
    color: bg

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    readonly property color bg: "#0e0e10"
    readonly property color surface: "#151518"
    readonly property color raised: "#1d1d22"
    readonly property color line: "#26262c"
    readonly property color ink: "#f4f4f5"
    readonly property color muted: "#a1a1aa"
    readonly property color faint: "#71717a"
    readonly property color warn: "#f5c16c"
    readonly property color bad: "#ff6b6b"
    readonly property int smallRadius: Math.min(4, theme.radius)
    readonly property bool wide: width >= 820

    property bool helpVisible: false
    property double now: Date.now()
    readonly property var providerOrder: ["claude", "codex", "grok", "cursor", "opencode", "antigravity", "pi", "muse"]
    readonly property var generalNotices: {
        var list = []
        for (var i = 0; i < backend.notices.length; ++i) {
            var code = backend.notices[i]
            if (code.indexOf("claude-") === 0 || code.indexOf("codex-") === 0 || code.indexOf("grok-") === 0
                    || code === "limits-busy" || noticeText(code).length === 0)
                continue
            list.push(code)
        }
        return list
    }
    readonly property var sortedModels: backend.models.slice().sort(function (a, b) { return metricValue(b) - metricValue(a) })
    readonly property real bucketPeak: {
        var peak = 0
        for (var i = 0; i < backend.buckets.length; ++i)
            peak = Math.max(peak, backend.buckets[i].value)
        return peak
    }
    readonly property var usageStats: {
        unusedLanguage()
        var hourly = backend.buckets.length > 0 && backend.buckets[0].day === undefined
        var total = backend.metric === "tokens" ? backend.totalTokens : backend.totalCost
        var busiest = null
        for (var i = 0; i < backend.buckets.length; ++i)
            if (backend.buckets[i].value > 0 && (!busiest || backend.buckets[i].value > busiest.value))
                busiest = backend.buckets[i]
        var count = Math.max(1, backend.buckets.length)
        return [
            { title: hourly ? qsTr("Hourly average") : qsTr("Daily average"), value: formatMetric(total / count), sub: "" },
            { title: hourly ? qsTr("Busiest hour") : qsTr("Busiest day"), value: busiest ? formatMetric(busiest.value) : "–",
              sub: busiest ? bucketRange(busiest, "ddd d MMM") : "" },
            backend.metric === "tokens"
                ? { title: qsTr("Estimated cost"), value: formatUsd(backend.totalCost), sub: "" }
                : { title: qsTr("Tokens"), value: formatTokens(backend.totalTokens), sub: "" }
        ]
    }
    readonly property real modelPeak: {
        var peak = 0
        for (var i = 0; i < backend.models.length; ++i)
            peak = Math.max(peak, metricValue(backend.models[i]))
        return peak
    }

    function unusedLanguage() { return translator.language }

    function metricValue(entry) {
        return backend.metric === "tokens" ? entry.tokens : entry.cost
    }

    function formatMetric(value) {
        return backend.metric === "tokens" ? formatTokens(value) : formatUsd(value)
    }

    function formatUsd(value) {
        unusedLanguage()
        return qsTr("%1 USD").arg(Number(value).toLocaleString(Qt.locale(), "f", 2))
    }

    function formatTokens(value) {
        unusedLanguage()
        var locale = Qt.locale()
        var n = Number(value)
        if (n >= 1000000000)
            return qsTr("%1 B").arg((n / 1000000000).toLocaleString(locale, "f", 2))
        if (n >= 1000000)
            return qsTr("%1 M").arg((n / 1000000).toLocaleString(locale, "f", 1))
        if (n >= 1000)
            return qsTr("%1 k").arg((n / 1000).toLocaleString(locale, "f", 1))
        return Math.round(n).toLocaleString(locale, "f", 0)
    }

    // Every axis label uses the decimals the half step needs, so 0, 5, and 10 read alike.
    function formatAxis(value, step) {
        unusedLanguage()
        if (backend.metric === "tokens")
            return formatTokens(value)
        var decimals = 2
        for (var d = 0; d < 2; ++d) {
            var scaled = step * Math.pow(10, d)
            if (Math.abs(scaled - Math.round(scaled)) < 1e-6) {
                decimals = d
                break
            }
        }
        return qsTr("%1 USD").arg(Number(value).toLocaleString(Qt.locale(), "f", decimals))
    }

    // Rounds a peak up to 1, 2, 2.5, or 5 times a power of ten, so the axis reads in round steps.
    function niceMax(peak) {
        if (peak <= 0)
            return 0
        var step = Math.pow(10, Math.floor(Math.log(peak) / Math.LN10))
        var f = peak / step
        return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * step
    }

    function tickIndexes(count) {
        if (count <= 0)
            return []
        if (count <= 7)
            return Array.from({ length: count }, function (_, i) { return i })
        var ticks = []
        for (var k = 0; k < 5; ++k)
            ticks.push(Math.round(k * (count - 1) / 4))
        return ticks
    }

    function bucketLabel(entry, dayPattern) {
        unusedLanguage()
        if (!entry)
            return ""
        if (entry.day !== undefined)
            return formatDay(entry.day, dayPattern)
        return new Date(entry.start).toLocaleTimeString(Qt.locale(), "HH:mm")
    }

    function bucketRange(entry, dayPattern) {
        unusedLanguage()
        if (!entry || entry.day !== undefined)
            return bucketLabel(entry, dayPattern)
        return qsTr("%1 – %2").arg(bucketLabel(entry)).arg(new Date(entry.start + 3600000).toLocaleTimeString(Qt.locale(), "HH:mm"))
    }

    function capitalize(text) {
        return text.length > 0 ? text.charAt(0).toUpperCase() + text.slice(1) : text
    }

    function formatPercent(value) {
        unusedLanguage()
        return qsTr("%1%").arg(Number(value).toLocaleString(Qt.locale(), "f", 0))
    }

    function formatWhen(iso) {
        unusedLanguage()
        if (!iso)
            return ""
        var date = new Date(iso)
        if (isNaN(date.getTime()))
            return ""
        var today = new Date(win.now)
        if (date.toDateString() === today.toDateString())
            return date.toLocaleTimeString(Qt.locale(), "HH:mm")
        return date.toLocaleString(Qt.locale(), "ddd d MMM, HH:mm")
    }

    function formatSpan(minutes) {
        unusedLanguage()
        var days = Math.floor(minutes / 1440)
        var hours = Math.floor((minutes % 1440) / 60)
        var mins = minutes % 60
        if (days > 0)
            return qsTr("%1 d %2 h").arg(days).arg(hours)
        if (hours > 0)
            return qsTr("%1 h %2 min").arg(hours).arg(mins)
        return qsTr("%1 min").arg(Math.max(1, mins))
    }

    function formatAgo(ms) {
        unusedLanguage()
        var minutes = Math.floor((win.now - ms) / 60000)
        if (minutes < 1)
            return qsTr("just now")
        if (minutes < 60)
            return qsTr("%1 min ago").arg(minutes)
        return formatWhen(new Date(ms).toISOString())
    }

    function formatCountdown(iso) {
        unusedLanguage()
        var date = new Date(iso)
        if (!iso || isNaN(date.getTime()))
            return ""
        var minutes = Math.round((date.getTime() - win.now) / 60000)
        if (minutes <= 0)
            return qsTr("Resets now")
        return qsTr("Resets in %1").arg(formatSpan(minutes))
    }

    function formatDay(isoDay, pattern) {
        unusedLanguage()
        var parts = String(isoDay).split("-")
        if (parts.length !== 3)
            return ""
        return new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2])).toLocaleDateString(Qt.locale(), pattern || "ddd d MMM")
    }

    function rangeText(days) {
        unusedLanguage()
        if (days === 1) return qsTr("past 24 hours")
        if (days === 7) return qsTr("past 7 days")
        if (days === 30) return qsTr("past 30 days")
        return qsTr("past 90 days")
    }

    // Window lengths for the pace estimate. Kinds without a fixed length get no estimate.
    function windowLength(kind) {
        if (kind === "session") return 5 * 3600000
        if (kind === "weekly") return 7 * 86400000
        if (kind === "monthly" || kind === "credit") return 30 * 86400000
        return 0
    }

    // Projects the current burn rate to the reset. Returns the elapsed share of the
    // window and, when the limit runs out first, the minutes until it does.
    function pace(row) {
        var length = windowLength(row.kind)
        var reset = new Date(row.resets).getTime()
        if (!length || isNaN(reset))
            return { elapsed: -1, outIn: -1 }
        var remaining = reset - win.now
        if (remaining <= 0 || remaining > length)
            return { elapsed: -1, outIn: -1 }
        var elapsedMs = length - remaining
        var elapsed = elapsedMs / length
        if (elapsed < 0.05 || row.used <= 0 || row.used >= 100)
            return { elapsed: elapsed, outIn: -1 }
        var untilEmpty = (100 - row.used) / (row.used / elapsedMs)
        // Running out an hour before a weekly reset is still on pace, so warn only
        // when the limit empties at least a tenth of the window early.
        return { elapsed: elapsed, outIn: untilEmpty < remaining - length * 0.1 ? Math.round(untilEmpty / 60000) : -1 }
    }

    function leftColor(left) {
        if (left <= 10) return bad
        if (left <= 25) return warn
        return ink
    }

    function providerLabel(id) {
        unusedLanguage()
        if (id === "claude") return qsTr("Claude Code")
        if (id === "codex") return qsTr("Codex")
        if (id === "grok") return qsTr("Grok Build")
        if (id === "cursor") return qsTr("Cursor")
        if (id === "opencode") return qsTr("OpenCode")
        if (id === "antigravity") return qsTr("Antigravity")
        if (id === "pi") return qsTr("Pi")
        if (id === "muse") return qsTr("Muse Code")
        return id
    }

    function providerColor(id) {
        if (id === "claude") return "#d97757"
        if (id === "codex") return "#e7e7ea"
        if (id === "grok") return "#c4b5a0"
        if (id === "pi") return "#4d9f8a"
        if (id === "opencode") return "#5b9bbd"
        if (id === "antigravity") return "#8c7bd1"
        if (id === "muse") return "#6ea8ff"
        return theme.accent
    }

    function windowLabel(kind, extra) {
        unusedLanguage()
        var base = qsTr("Limit")
        if (kind === "session") base = qsTr("Session")
        else if (kind === "weekly") base = qsTr("Weekly")
        else if (kind === "monthly") base = qsTr("Monthly")
        else if (kind === "spend") base = qsTr("Monthly spend")
        else if (kind === "credit") base = qsTr("Monthly credit limit")
        else if (kind === "subscription") base = qsTr("Subscription")
        return extra ? qsTr("%1 · %2").arg(base).arg(extra) : base
    }

    function noticeText(code) {
        unusedLanguage()
        if (code === "no-cache")
            return qsTr("T3 Code has no usage cache. Open T3 Code, then press Refresh.")
        if (code === "no-rates")
            return qsTr("Model prices are missing. Cost stays at 0.")
        if (code === "empty")
            return qsTr("No usage in this window.")
        if (code === "unpriced")
            return qsTr("Some models have no price. Their cost is 0.")
        if (code === "claude-login")
            return qsTr("Claude login is missing. Sign in with Claude Code, then press Refresh.")
        if (code === "claude-failed")
            return qsTr("Claude limits check failed. Press Refresh.")
        if (code === "claude-empty")
            return qsTr("Claude answered, but sent no limit windows.")
        if (code === "claude-http-401")
            return qsTr("Claude login expired. Open Claude Code to renew it, then press Refresh.")
        if (code.indexOf("claude-http-") === 0)
            return qsTr("Claude limits check failed. HTTP %1.").arg(code.slice(12))
        if (code === "codex-failed")
            return qsTr("Codex did not report limits. Press Refresh.")
        if (code === "grok-login")
            return qsTr("Grok login is missing.")
        if (code === "grok-failed")
            return qsTr("Grok limits check failed. Press Refresh.")
        if (code === "grok-key")
            return qsTr("Grok uses an API key. No subscription limit.")
        if (code === "limits-busy")
            return qsTr("Limits check in progress.")
        return ""
    }

    function limitsFor(id) {
        var rows = []
        for (var i = 0; i < backend.limits.length; ++i) {
            if (backend.limits[i].provider === id)
                rows.push(backend.limits[i])
        }
        return rows
    }

    function providerNotice(id) {
        for (var i = 0; i < backend.notices.length; ++i) {
            var code = backend.notices[i]
            if (id === "claude" && (code === "claude-login" || code === "claude-failed" || code === "claude-empty" || code.indexOf("claude-http-") === 0))
                return noticeText(code)
            if (id === "codex" && code === "codex-failed")
                return noticeText(code)
            if (id === "grok" && (code === "grok-login" || code === "grok-failed" || code === "grok-key"))
                return noticeText(code)
        }
        return ""
    }

    Timer {
        interval: 30000
        running: true
        repeat: true
        onTriggered: win.now = Date.now()
    }

    Connections {
        target: backend
        function onDataChanged() { win.now = Date.now() }
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: backend.primaryAction() }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; onActivated: backend.setMetric("cost") }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; onActivated: backend.setMetric("tokens") }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; onActivated: meterView.contentY = 0 }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; onActivated: backend.setWindowDays(1) }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; onActivated: backend.setWindowDays(7) }
    Shortcut { sequence: "3"; context: Qt.ApplicationShortcut; onActivated: backend.setWindowDays(30) }
    Shortcut { sequence: "4"; context: Qt.ApplicationShortcut; onActivated: backend.setWindowDays(90) }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = false }

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            anchors.bottomMargin: 0
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: 20
                spacing: 8

                ColumnLayout {
                    spacing: 2
                    Label {
                        text: qsTr("Quota")
                        color: win.ink
                        font.pixelSize: 22
                        font.bold: true
                        font.letterSpacing: -0.4
                    }
                    Row {
                        spacing: 6
                        Rectangle {
                            id: busyDot
                            width: 6
                            height: 6
                            radius: 3
                            anchors.verticalCenter: parent.verticalCenter
                            color: backend.busy ? theme.accent : win.faint
                            SequentialAnimation on opacity {
                                running: backend.busy
                                loops: Animation.Infinite
                                onRunningChanged: if (!running) busyDot.opacity = 1
                                NumberAnimation { to: 0.25; duration: 500 }
                                NumberAnimation { to: 1; duration: 500 }
                            }
                        }
                        Label {
                            text: backend.busy ? qsTr("Checking limits…")
                                : backend.limitsAt > 0 ? qsTr("Limits checked %1").arg(win.formatAgo(backend.limitsAt))
                                : qsTr("Limits not checked yet")
                            color: win.muted
                            font.pixelSize: 12
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    implicitWidth: refreshRow.implicitWidth + 24
                    implicitHeight: 32
                    radius: win.smallRadius
                    color: refreshArea.containsMouse && !backend.busy ? win.raised : "transparent"
                    border.color: refreshArea.containsMouse && !backend.busy ? theme.accent : win.line
                    opacity: backend.busy ? 0.55 : 1
                    Row {
                        id: refreshRow
                        anchors.centerIn: parent
                        spacing: 8
                        Label {
                            text: qsTr("Refresh")
                            color: win.ink
                            font.pixelSize: 13
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            text: "Space"
                            color: win.faint
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        id: refreshArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !backend.busy
                        cursorShape: Qt.PointingHandCursor
                        onClicked: backend.refresh()
                    }
                }

                Rectangle {
                    implicitWidth: 32
                    implicitHeight: 32
                    radius: win.smallRadius
                    color: helpArea.containsMouse ? win.raised : "transparent"
                    border.color: helpArea.containsMouse ? theme.accent : win.line
                    Label {
                        anchors.centerIn: parent
                        text: "?"
                        color: win.ink
                        font.pixelSize: 14
                    }
                    MouseArea {
                        id: helpArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: win.helpVisible = true
                    }
                }
            }

            Flickable {
                id: meterView
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentHeight: board.implicitHeight + 24
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; width: 8 }

                ColumnLayout {
                    id: board
                    width: meterView.width
                    spacing: 12

                    Label {
                        text: qsTr("Limits")
                        color: win.faint
                        font.pixelSize: 11
                        font.bold: true
                        font.letterSpacing: 1.2
                        font.capitalization: Font.AllUppercase
                    }

                    Repeater {
                        model: win.providerOrder
                        delegate: Rectangle {
                            id: card
                            required property string modelData
                            readonly property var rows: win.limitsFor(modelData)
                            readonly property string problem: win.providerNotice(modelData)
                            // An API key is a choice, not a failure, so it reads as plain information.
                            readonly property bool info: modelData === "grok" && backend.notices.indexOf("grok-key") >= 0
                            visible: rows.length > 0 || problem.length > 0
                            Layout.fillWidth: true
                            implicitHeight: cardBody.implicitHeight + 40
                            radius: theme.radius
                            color: win.surface
                            border.color: win.line

                            ColumnLayout {
                                id: cardBody
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 16

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Rectangle {
                                        implicitWidth: 10
                                        implicitHeight: 10
                                        radius: Math.min(5, theme.radius)
                                        color: win.providerColor(card.modelData)
                                    }
                                    Label {
                                        text: win.providerLabel(card.modelData)
                                        color: win.ink
                                        font.pixelSize: 15
                                        font.bold: true
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        visible: card.problem.length > 0 && card.rows.length > 0 && !card.info
                                        text: qsTr("Last known values")
                                        color: win.warn
                                        font.pixelSize: 12
                                    }
                                }

                                Label {
                                    visible: card.problem.length > 0
                                    Layout.fillWidth: true
                                    text: card.problem
                                    color: card.rows.length > 0 || card.info ? win.muted : win.warn
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 13
                                }

                                GridLayout {
                                    visible: card.rows.length > 0
                                    Layout.fillWidth: true
                                    columns: win.wide ? 2 : 1
                                    columnSpacing: 32
                                    rowSpacing: 20

                                    Repeater {
                                        model: card.rows
                                        delegate: ColumnLayout {
                                            id: tile
                                            required property var modelData
                                            readonly property var estimate: win.pace(modelData)
                                            readonly property color fill: modelData.left <= 10 ? win.bad : win.providerColor(card.modelData)
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 1
                                            Layout.alignment: Qt.AlignTop
                                            spacing: 8

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6
                                                Label {
                                                    text: win.windowLabel(tile.modelData.kind, tile.modelData.extra)
                                                    color: win.muted
                                                    font.pixelSize: 13
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                    Layout.alignment: Qt.AlignBaseline
                                                }
                                                Label {
                                                    text: win.formatPercent(tile.modelData.left)
                                                    color: win.leftColor(tile.modelData.left)
                                                    font.pixelSize: 24
                                                    font.bold: true
                                                    font.letterSpacing: -0.4
                                                    font.features: { "tnum": 1 }
                                                    Layout.alignment: Qt.AlignBaseline
                                                }
                                                Label {
                                                    text: qsTr("left")
                                                    color: win.muted
                                                    font.pixelSize: 13
                                                    Layout.alignment: Qt.AlignBaseline
                                                }
                                            }

                                            Item {
                                                Layout.fillWidth: true
                                                implicitHeight: 14
                                                Rectangle {
                                                    id: track
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: parent.width
                                                    height: 8
                                                    radius: Math.min(4, theme.radius)
                                                    color: win.raised
                                                    Rectangle {
                                                        width: tile.modelData.used <= 0 ? 0 : Math.max(3, parent.width * Math.min(1, tile.modelData.used / 100))
                                                        height: parent.height
                                                        radius: parent.radius
                                                        color: tile.fill
                                                        Behavior on width { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
                                                    }
                                                }
                                                // Marks how much of the window has passed. A fill past the mark burns faster than the reset.
                                                Rectangle {
                                                    visible: tile.estimate.elapsed > 0
                                                    x: Math.round(track.width * tile.estimate.elapsed) - 1
                                                    width: 2
                                                    height: parent.height
                                                    radius: 1
                                                    color: win.ink
                                                    opacity: 0.6
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Label {
                                                    text: tile.modelData.resets.length > 0 ? win.formatCountdown(tile.modelData.resets) : qsTr("%1 used").arg(win.formatPercent(tile.modelData.used))
                                                    color: win.muted
                                                    font.pixelSize: 12
                                                    font.features: { "tnum": 1 }
                                                }
                                                Label {
                                                    visible: tile.modelData.resets.length > 0
                                                    text: win.formatWhen(tile.modelData.resets)
                                                    color: win.faint
                                                    font.pixelSize: 12
                                                }
                                                Label {
                                                    visible: tile.estimate.outIn >= 0
                                                    text: qsTr("Runs out in %1").arg(win.formatSpan(tile.estimate.outIn))
                                                    color: win.warn
                                                    font.pixelSize: 12
                                                    elide: Text.ElideLeft
                                                    Layout.fillWidth: true
                                                    horizontalAlignment: Text.AlignRight
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        spacing: 10
                        Label {
                            text: qsTr("Usage")
                            color: win.faint
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 1.2
                            font.capitalization: Font.AllUppercase
                            Layout.fillWidth: true
                        }
                        Repeater {
                            model: (translator.language, [
                                [{ key: "cost", label: qsTr("Cost") }, { key: "tokens", label: qsTr("Tokens") }],
                                [{ key: 1, label: qsTr("24 h") }, { key: 7, label: qsTr("7 d") },
                                 { key: 30, label: qsTr("30 d") }, { key: 90, label: qsTr("90 d") }]
                            ])
                            delegate: Rectangle {
                                id: group
                                required property var modelData
                                required property int index
                                implicitWidth: segments.implicitWidth + 6
                                implicitHeight: 30
                                radius: win.smallRadius
                                color: win.surface
                                border.color: win.line

                                Row {
                                    id: segments
                                    anchors.centerIn: parent
                                    spacing: 2
                                    Repeater {
                                        model: group.modelData
                                        delegate: Rectangle {
                                            required property var modelData
                                            readonly property bool selected: group.index === 0 ? backend.metric === modelData.key
                                                                                               : backend.windowDays === modelData.key
                                            width: Math.max(40, segmentLabel.implicitWidth + 18)
                                            height: 24
                                            radius: Math.max(0, win.smallRadius - 1)
                                            color: selected ? theme.accent : segmentArea.containsMouse ? win.raised : "transparent"
                                            Label {
                                                id: segmentLabel
                                                anchors.centerIn: parent
                                                text: modelData.label
                                                color: selected ? theme.accentForeground : win.muted
                                                font.pixelSize: 12
                                                font.bold: selected
                                            }
                                            MouseArea {
                                                id: segmentArea
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: group.index === 0 ? backend.setMetric(modelData.key)
                                                                             : backend.setWindowDays(modelData.key)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: usageBody.implicitHeight + 48
                        radius: theme.radius
                        color: win.surface
                        border.color: win.line

                        ColumnLayout {
                            id: usageBody
                            anchors.fill: parent
                            anchors.margins: 24
                            spacing: 28

                            ColumnLayout {
                                visible: win.generalNotices.length > 0
                                Layout.fillWidth: true
                                Layout.bottomMargin: -8
                                spacing: 6
                                Repeater {
                                    model: win.generalNotices
                                    delegate: RowLayout {
                                        required property string modelData
                                        Layout.fillWidth: true
                                        spacing: 10
                                        Rectangle {
                                            implicitWidth: 6
                                            implicitHeight: 6
                                            radius: 3
                                            color: win.warn
                                        }
                                        Label {
                                            text: win.noticeText(modelData)
                                            color: win.warn
                                            font.pixelSize: 13
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: win.wide ? 4 : 3
                                columnSpacing: 32
                                rowSpacing: 20

                                ColumnLayout {
                                    Layout.columnSpan: win.wide ? 1 : 3
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 2
                                    spacing: 2
                                    Label {
                                        text: backend.metric === "tokens" ? qsTr("Tokens") : qsTr("Estimated API cost")
                                        color: win.muted
                                        font.pixelSize: 12
                                    }
                                    Label {
                                        text: win.formatMetric(backend.metric === "tokens" ? backend.totalTokens : backend.totalCost)
                                        color: win.ink
                                        font.pixelSize: 34
                                        font.bold: true
                                        font.letterSpacing: -0.8
                                        font.features: { "tnum": 1 }
                                    }
                                    Label {
                                        text: backend.metric === "tokens" ? win.capitalize(win.rangeText(backend.windowDays))
                                            : qsTr("%1. Not the subscription bill.").arg(win.capitalize(win.rangeText(backend.windowDays)))
                                        color: win.faint
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                }

                                Repeater {
                                    model: win.usageStats
                                    delegate: ColumnLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        Layout.alignment: Qt.AlignTop
                                        spacing: 2
                                        Label {
                                            text: modelData.title
                                            color: win.muted
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: modelData.value
                                            color: win.ink
                                            font.pixelSize: 18
                                            font.bold: true
                                            font.features: { "tnum": 1 }
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: modelData.sub
                                            color: win.faint
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }

                            Item {
                                id: chart
                                objectName: "usageChart"
                                visible: backend.buckets.length > 0
                                Layout.fillWidth: true
                                implicitHeight: 190
                                readonly property int count: backend.buckets.length
                                readonly property bool hourly: count > 0 && backend.buckets[0].day === undefined
                                readonly property real gutter: 64
                                readonly property real plotTop: 8
                                readonly property real plotBottom: height - 24
                                readonly property real plotHeight: plotBottom - plotTop
                                readonly property real slot: count > 0 ? (width - gutter) / count : 0
                                readonly property real barWidth: Math.max(2, Math.min(36, slot * (count > 31 ? 0.72 : 0.6)))
                                readonly property real scale: win.niceMax(win.bucketPeak)
                                property int hovered: -1

                                Repeater {
                                    model: [1, 0.5, 0]
                                    delegate: Item {
                                        required property real modelData
                                        y: chart.plotTop + chart.plotHeight * (1 - modelData)
                                        width: chart.width
                                        Rectangle {
                                            x: chart.gutter
                                            width: chart.width - chart.gutter
                                            height: 1
                                            color: modelData === 0 ? win.line : Qt.rgba(1, 1, 1, 0.05)
                                        }
                                        Label {
                                            visible: chart.scale > 0
                                            width: chart.gutter - 12
                                            anchors.verticalCenter: parent.top
                                            horizontalAlignment: Text.AlignRight
                                            text: win.formatAxis(chart.scale * modelData, chart.scale / 2)
                                            color: win.faint
                                            font.pixelSize: 11
                                            font.features: { "tnum": 1 }
                                        }
                                    }
                                }

                                Repeater {
                                    model: backend.buckets
                                    delegate: Item {
                                        id: bucketItem
                                        required property var modelData
                                        required property int index
                                        x: chart.gutter + index * chart.slot
                                        y: chart.plotTop
                                        width: chart.slot
                                        height: chart.plotHeight
                                        opacity: chart.hovered >= 0 && chart.hovered !== index ? 0.35 : 1
                                        Behavior on opacity { NumberAnimation { duration: 120 } }

                                        Rectangle {
                                            visible: bucketItem.modelData.value <= 0
                                            anchors.bottom: parent.bottom
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: chart.barWidth
                                            height: 2
                                            color: win.raised
                                        }

                                        Column {
                                            anchors.bottom: parent.bottom
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: chart.barWidth
                                            Repeater {
                                                model: bucketItem.modelData.parts.slice().reverse()
                                                delegate: Rectangle {
                                                    required property var modelData
                                                    required property int index
                                                    width: chart.barWidth
                                                    height: chart.scale > 0 ? chart.plotHeight * modelData.value / chart.scale : 0
                                                    color: win.providerColor(modelData.provider)
                                                    topLeftRadius: index === 0 ? Math.min(3, theme.radius, width / 2) : 0
                                                    topRightRadius: topLeftRadius
                                                }
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onEntered: chart.hovered = bucketItem.index
                                            onExited: if (chart.hovered === bucketItem.index) chart.hovered = -1
                                        }
                                    }
                                }

                                Repeater {
                                    model: win.tickIndexes(chart.count)
                                    delegate: Label {
                                        required property int modelData
                                        readonly property real center: chart.gutter + (modelData + 0.5) * chart.slot
                                        x: Math.max(chart.gutter, Math.min(chart.width - width, center - width / 2))
                                        y: chart.plotBottom + 8
                                        text: win.bucketLabel(backend.buckets[modelData], chart.count <= 7 ? "ddd" : "d MMM")
                                        color: win.faint
                                        font.pixelSize: 11
                                    }
                                }

                                Rectangle {
                                    id: tip
                                    readonly property var entry: chart.hovered >= 0 && chart.hovered < chart.count ? backend.buckets[chart.hovered] : null
                                    readonly property real center: chart.gutter + (chart.hovered + 0.5) * chart.slot
                                    visible: entry !== null
                                    width: tipBody.implicitWidth + 24
                                    height: tipBody.implicitHeight + 20
                                    x: center + chart.barWidth / 2 + 8 + width <= chart.width ? center + chart.barWidth / 2 + 8
                                                                                          : center - chart.barWidth / 2 - 8 - width
                                    y: chart.plotTop
                                    radius: win.smallRadius
                                    color: win.raised
                                    border.color: win.line

                                    ColumnLayout {
                                        id: tipBody
                                        anchors.centerIn: parent
                                        spacing: 6
                                        Label {
                                            text: tip.entry ? win.bucketRange(tip.entry, "dddd d MMM") : ""
                                            color: win.muted
                                            font.pixelSize: 11
                                        }
                                        Label {
                                            text: tip.entry ? win.formatMetric(tip.entry.value) : ""
                                            color: win.ink
                                            font.pixelSize: 15
                                            font.bold: true
                                            font.features: { "tnum": 1 }
                                        }
                                        Repeater {
                                            model: tip.entry ? tip.entry.parts : []
                                            delegate: RowLayout {
                                                required property var modelData
                                                spacing: 8
                                                Rectangle {
                                                    implicitWidth: 6
                                                    implicitHeight: 6
                                                    radius: 3
                                                    color: win.providerColor(modelData.provider)
                                                }
                                                Label {
                                                    text: win.providerLabel(modelData.provider)
                                                    color: win.muted
                                                    font.pixelSize: 12
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 90
                                                }
                                                Label {
                                                    text: win.formatMetric(modelData.value)
                                                    color: win.ink
                                                    font.pixelSize: 12
                                                    font.features: { "tnum": 1 }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            ColumnLayout {
                                visible: backend.rows.length > 0
                                Layout.fillWidth: true
                                spacing: 14

                                Label {
                                    text: qsTr("By provider")
                                    color: win.ink
                                    font.pixelSize: 13
                                    font.bold: true
                                }

                                Row {
                                    id: shareBar
                                    Layout.fillWidth: true
                                    height: 10
                                    spacing: 2
                                    readonly property real usable: width - spacing * Math.max(0, visibleShares - 1)
                                    readonly property int visibleShares: {
                                        var n = 0
                                        for (var i = 0; i < backend.rows.length; ++i)
                                            if (backend.rows[i].share > 0) ++n
                                        return n
                                    }
                                    Repeater {
                                        model: backend.rows
                                        delegate: Rectangle {
                                            required property var modelData
                                            visible: modelData.share > 0
                                            width: Math.max(2, shareBar.usable * modelData.share)
                                            height: shareBar.height
                                            radius: Math.min(2, theme.radius)
                                            color: win.providerColor(modelData.provider)
                                            Behavior on width { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
                                        }
                                    }
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: Math.max(1, Math.min(backend.rows.length, win.wide ? 4 : 2))
                                    columnSpacing: 24
                                    rowSpacing: 14
                                    Repeater {
                                        model: backend.rows
                                        delegate: ColumnLayout {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 1
                                            spacing: 2
                                            RowLayout {
                                                spacing: 8
                                                Rectangle {
                                                    implicitWidth: 8
                                                    implicitHeight: 8
                                                    radius: Math.min(4, theme.radius)
                                                    color: win.providerColor(modelData.provider)
                                                }
                                                Label {
                                                    text: win.providerLabel(modelData.provider)
                                                    color: win.muted
                                                    font.pixelSize: 12
                                                }
                                            }
                                            RowLayout {
                                                spacing: 8
                                                Label {
                                                    text: win.formatMetric(win.metricValue(modelData))
                                                    color: win.ink
                                                    font.pixelSize: 15
                                                    font.bold: true
                                                    font.features: { "tnum": 1 }
                                                    Layout.alignment: Qt.AlignBaseline
                                                }
                                                Label {
                                                    text: win.formatPercent(modelData.share * 100)
                                                    color: win.faint
                                                    font.pixelSize: 12
                                                    font.features: { "tnum": 1 }
                                                    Layout.alignment: Qt.AlignBaseline
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            ColumnLayout {
                                visible: backend.models.length > 0
                                Layout.fillWidth: true
                                spacing: 0

                                Label {
                                    text: qsTr("Top models")
                                    color: win.ink
                                    font.pixelSize: 13
                                    font.bold: true
                                    Layout.bottomMargin: 10
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 10
                                    Layout.rightMargin: 10
                                    Layout.bottomMargin: 6
                                    spacing: 16
                                    Label {
                                        text: qsTr("Model")
                                        color: win.faint
                                        font.pixelSize: 11
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        visible: win.wide
                                        text: qsTr("Provider")
                                        color: win.faint
                                        font.pixelSize: 11
                                        Layout.preferredWidth: 110
                                    }
                                    Label {
                                        text: qsTr("Tokens")
                                        color: backend.metric === "tokens" ? win.muted : win.faint
                                        font.pixelSize: 11
                                        horizontalAlignment: Text.AlignRight
                                        Layout.preferredWidth: 80
                                    }
                                    Label {
                                        text: qsTr("Cost")
                                        color: backend.metric === "cost" ? win.muted : win.faint
                                        font.pixelSize: 11
                                        horizontalAlignment: Text.AlignRight
                                        Layout.preferredWidth: 96
                                    }
                                }

                                Repeater {
                                    model: win.sortedModels
                                    delegate: Item {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        implicitHeight: 36

                                        Rectangle {
                                            width: parent.width
                                            height: 1
                                            color: win.line
                                        }
                                        // The faint fill behind each row is its share of the top model.
                                        Rectangle {
                                            y: 1
                                            width: win.modelPeak > 0 ? parent.width * win.metricValue(modelData) / win.modelPeak : 0
                                            height: parent.height - 1
                                            color: Qt.rgba(1, 1, 1, 0.035)
                                            Behavior on width { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
                                        }
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            spacing: 16
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 10
                                                Rectangle {
                                                    implicitWidth: 6
                                                    implicitHeight: 6
                                                    radius: 3
                                                    color: win.providerColor(modelData.provider)
                                                }
                                                Label {
                                                    text: modelData.model
                                                    color: win.ink
                                                    font.pixelSize: 13
                                                    elide: Text.ElideMiddle
                                                    Layout.fillWidth: true
                                                }
                                            }
                                            Label {
                                                visible: win.wide
                                                text: win.providerLabel(modelData.provider)
                                                color: win.muted
                                                font.pixelSize: 12
                                                elide: Text.ElideRight
                                                Layout.preferredWidth: 110
                                            }
                                            Label {
                                                text: win.formatTokens(modelData.tokens)
                                                color: backend.metric === "tokens" ? win.ink : win.muted
                                                font.pixelSize: 13
                                                font.features: { "tnum": 1 }
                                                horizontalAlignment: Text.AlignRight
                                                Layout.preferredWidth: 80
                                            }
                                            Label {
                                                text: win.formatUsd(modelData.cost)
                                                color: backend.metric === "cost" ? win.ink : win.muted
                                                font.pixelSize: 13
                                                font.features: { "tnum": 1 }
                                                horizontalAlignment: Text.AlignRight
                                                Layout.preferredWidth: 96
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            visible: win.helpVisible
            anchors.fill: parent
            color: "#d90e0e10"
            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

            Rectangle {
                anchors.centerIn: parent
                width: helpBody.implicitWidth + 56
                height: helpBody.implicitHeight + 48
                radius: theme.radius
                color: win.surface
                border.color: win.line
                MouseArea { anchors.fill: parent }

                ColumnLayout {
                    id: helpBody
                    anchors.centerIn: parent
                    spacing: 16
                    Label {
                        text: qsTr("Shortcuts")
                        color: win.ink
                        font.pixelSize: 18
                        font.bold: true
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 20
                        rowSpacing: 10
                        Repeater {
                            model: (translator.language, [
                                { keys: "Space", label: qsTr("Refresh") },
                                { keys: "C", label: qsTr("Show cost") },
                                { keys: "T", label: qsTr("Show tokens") },
                                { keys: "1 – 4", label: qsTr("24 hours, 7, 30, or 90 days") },
                                { keys: "L", label: qsTr("Scroll to limits") },
                                { keys: "?", label: qsTr("Show this list") },
                                { keys: "Esc", label: qsTr("Close this list") },
                                { keys: "Q", label: qsTr("Quit") }
                            ])
                            delegate: Item {
                                required property var modelData
                                Layout.columnSpan: 2
                                Layout.fillWidth: true
                                implicitWidth: 72 + 20 + helpLabel.implicitWidth
                                implicitHeight: 26
                                Rectangle {
                                    width: 72
                                    height: 26
                                    radius: win.smallRadius
                                    color: win.raised
                                    border.color: win.line
                                    Label {
                                        anchors.centerIn: parent
                                        text: modelData.keys
                                        color: win.ink
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }
                                Label {
                                    id: helpLabel
                                    x: 92
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.label
                                    color: win.muted
                                    font.pixelSize: 13
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
