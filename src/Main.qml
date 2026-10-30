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
    // T3 Code's provider order and colors, so the chart reads the same in both apps.
    readonly property var chartOrder: ["codex", "claude", "grok", "cursor", "opencode", "antigravity", "pi", "muse"]
    readonly property string grokColor: mixOklab(ink, bg, 0.72).toString()
    readonly property color chartGrid: Qt.rgba(1, 1, 1, 0.08)
    // A locale that writes "AP" in its short time format uses a 12-hour clock.
    readonly property bool hour12: /a/i.test(Qt.locale().timeFormat(Locale.ShortFormat))
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

    // The chart formats numbers as T3 Code does: en-US dollars and short token counts.
    function chartMetric(value) {
        if (backend.metric !== "tokens")
            return "$" + Number(value).toLocaleString(Qt.locale("en_US"), "f", 2)
        var n = Number(value)
        var units = [[1e12, "T"], [1e9, "B"], [1e6, "M"], [1e3, "K"]]
        for (var i = 0; i < units.length; ++i) {
            if (Math.abs(n) < units[i][0])
                continue
            var scaled = n / units[i][0]
            var digits = Math.abs(scaled) >= 100 ? 0 : Math.abs(scaled) >= 10 ? 1 : 2
            return scaled.toFixed(digits).replace(/\.0+$/, "") + units[i][1]
        }
        return Math.round(n).toLocaleString(Qt.locale("en_US"), "f", 0)
    }

    function chartHour(ms) {
        var hour = new Date(ms).getHours()
        if (!hour12)
            return (hour < 10 ? "0" : "") + hour + ":00"
        return (hour % 12 === 0 ? 12 : hour % 12) + (hour < 12 ? " AM" : " PM")
    }

    function chartDay(date) {
        var months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        return months[date.getMonth()] + " " + date.getDate()
    }

    function bucketDate(entry) {
        if (entry.day === undefined)
            return new Date(entry.start)
        var parts = String(entry.day).split("-")
        return new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2]))
    }

    function chartPeriod(entry) {
        if (!entry)
            return ""
        return entry.day === undefined ? chartHour(entry.start) : chartDay(bucketDate(entry))
    }

    // "3 PM today", "3 PM yesterday", then "Oct 27, 3 PM" for older hours.
    function chartTooltipPeriod(entry) {
        if (!entry || entry.day !== undefined)
            return chartPeriod(entry)
        var day = bucketDate(entry)
        var today = new Date(win.now)
        var daysAgo = Math.round((new Date(today.getFullYear(), today.getMonth(), today.getDate())
                                  - new Date(day.getFullYear(), day.getMonth(), day.getDate())) / 86400000)
        if (daysAgo === 0)
            return qsTr("%1 today").arg(chartHour(entry.start))
        if (daysAgo === 1)
            return qsTr("%1 yesterday").arg(chartHour(entry.start))
        return chartDay(day) + ", " + chartHour(entry.start)
    }

    // Steps of 1, 2, 5, or 10 times a power of ten, about `count` steps up to the peak.
    function niceScale(peak, count) {
        if (peak <= 0)
            return { max: 0, ticks: [0] }
        var raw = peak / count
        var magnitude = Math.pow(10, Math.floor(Math.log10(raw)))
        var normalized = raw / magnitude
        var step = (normalized > 5 ? 10 : normalized > 2 ? 5 : normalized > 1 ? 2 : 1) * magnitude
        var max = Math.ceil(peak / step) * step
        var ticks = []
        for (var value = 0; value <= max + step * 1e-6; value += step)
            ticks.push(value)
        return { max: max, ticks: ticks }
    }

    // Monotone cubic segments, so a curve never overshoots below zero or above a peak.
    function smoothCurve(points) {
        var count = points.length
        if (count < 2)
            return []
        var slopes = []
        for (var i = 0; i < count - 1; ++i) {
            var dx = points[i + 1].x - points[i].x
            slopes.push(dx === 0 ? 0 : (points[i + 1].y - points[i].y) / dx)
        }
        var tangents = Array.from({ length: count }, function () { return 0 })
        tangents[0] = slopes[0]
        tangents[count - 1] = slopes[count - 2]
        for (var j = 1; j < count - 1; ++j)
            tangents[j] = slopes[j - 1] * slopes[j] <= 0 ? 0 : (slopes[j - 1] + slopes[j]) / 2
        for (var k = 0; k < count - 1; ++k) {
            if (slopes[k] === 0) {
                tangents[k] = 0
                tangents[k + 1] = 0
                continue
            }
            var a = tangents[k] / slopes[k]
            var b = tangents[k + 1] / slopes[k]
            var magnitude = a * a + b * b
            if (magnitude > 9) {
                var scale = 3 / Math.sqrt(magnitude)
                tangents[k] = scale * a * slopes[k]
                tangents[k + 1] = scale * b * slopes[k]
            }
        }
        var segments = []
        for (var s = 0; s < count - 1; ++s) {
            var from = points[s]
            var to = points[s + 1]
            var step = to.x - from.x
            segments.push({ from: from, to: to,
                            c1: { x: from.x + step / 3, y: from.y + tangents[s] * step / 3 },
                            c2: { x: to.x - step / 3, y: to.y - tangents[s + 1] * step / 3 } })
        }
        return segments
    }

    // CSS color-mix(in oklab, a weight, b), which T3 Code uses for the Grok color.
    function mixOklab(a, b, weight) {
        function linear(c) { return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4) }
        function encode(c) {
            c = Math.max(0, Math.min(1, c))
            return c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055
        }
        function lab(c) {
            var r = linear(c.r), g = linear(c.g), bl = linear(c.b)
            var l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * bl)
            var m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * bl)
            var s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * bl)
            return [0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                    1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                    0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s]
        }
        var x = lab(a), y = lab(b)
        var L = x[0] * weight + y[0] * (1 - weight)
        var A = x[1] * weight + y[1] * (1 - weight)
        var B = x[2] * weight + y[2] * (1 - weight)
        var l3 = Math.pow(L + 0.3963377774 * A + 0.2158037573 * B, 3)
        var m3 = Math.pow(L - 0.1055613458 * A - 0.0638541728 * B, 3)
        var s3 = Math.pow(L - 0.0894841775 * A - 1.2914855480 * B, 3)
        return Qt.rgba(encode(4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3),
                       encode(-1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3),
                       encode(-0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3), 1)
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
        if (id === "codex") return ink.toString()
        if (id === "grok") return grokColor
        if (id === "cursor") return "#8b8b8b"
        if (id === "pi") return "#4d9f8a"
        if (id === "opencode") return "#5b9bbd"
        if (id === "antigravity") return "#8c7bd1"
        if (id === "muse") return "#0866ff"
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
            return qsTr("No usage history on this computer.")
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
        if (code.indexOf("codex-failed:") === 0)
            return qsTr("Codex limits unavailable for %1. Sign in to that profile and refresh.").arg(code.slice(13))
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

    function sourceText(source) {
        unusedLanguage()
        if (source === "local+t3")
            return qsTr("From local history and T3 Code")
        if (source === "local")
            return qsTr("From local history")
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

    function accountLabel(path) {
        var name = path.split("/").pop()
        if (name === ".codex")
            return qsTr("Default")
        name = name.replace(/^\.?codex[-_]?/i, "").replace(/[-_]+/g, " ").trim()
        if (!name)
            return qsTr("Default")
        if (/^[0-9]+$/.test(name))
            return qsTr("Account %1").arg(name)
        return name.charAt(0).toUpperCase() + name.slice(1)
    }

    function limitCards() {
        var cards = []
        for (var p = 0; p < providerOrder.length; ++p) {
            var provider = providerOrder[p]
            var rows = limitsFor(provider)
            if (provider !== "codex") {
                cards.push({ provider: provider, account: "", title: providerLabel(provider),
                               rows: rows, problem: providerNotice(provider) })
                continue
            }
            var accounts = []
            for (var r = 0; r < rows.length; ++r) {
                var account = rows[r].account || ""
                if (accounts.indexOf(account) < 0)
                    accounts.push(account)
            }
            for (var n = 0; n < backend.notices.length; ++n) {
                var code = backend.notices[n]
                if (code.indexOf("codex-failed:") === 0 && accounts.indexOf(code.slice(13)) < 0)
                    accounts.push(code.slice(13))
                else if (code === "codex-failed" && accounts.indexOf("") < 0)
                    accounts.push("")
            }
            accounts.sort()
            for (var a = 0; a < accounts.length; ++a) {
                var path = accounts[a]
                var accountRows = []
                for (var i = 0; i < rows.length; ++i)
                    if ((rows[i].account || "") === path)
                        accountRows.push(rows[i])
                var failed = backend.notices.indexOf(path ? "codex-failed:" + path : "codex-failed") >= 0
                cards.push({ provider: provider, account: path,
                               title: path ? accountLabel(path) : providerLabel(provider),
                               rows: accountRows,
                               problem: failed ? qsTr("Limits unavailable. Sign in to this Codex profile, then refresh.") : "" })
            }
        }
        return cards
    }

    function providerNotice(id) {
        for (var i = 0; i < backend.notices.length; ++i) {
            var code = backend.notices[i]
            if (id === "claude" && (code === "claude-login" || code === "claude-failed" || code === "claude-empty" || code.indexOf("claude-http-") === 0))
                return noticeText(code)
            if (id === "codex" && code.indexOf("codex-failed") === 0)
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
                        model: win.limitCards()
                        delegate: Rectangle {
                            id: card
                            required property var modelData
                            readonly property string provider: modelData.provider
                            readonly property var rows: modelData.rows
                            readonly property string problem: modelData.problem
                            // An API key is a choice, not a failure, so it reads as plain information.
                            readonly property bool info: provider === "grok" && backend.notices.indexOf("grok-key") >= 0
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
                                        color: win.providerColor(card.provider)
                                    }
                                    Label {
                                        text: card.modelData.title
                                        textFormat: Text.PlainText
                                        color: win.ink
                                        font.pixelSize: 15
                                        font.bold: true
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                        Accessible.name: text
                                        Accessible.description: card.modelData.account
                                        ToolTip.visible: accountHover.containsMouse && !!card.modelData.account
                                        ToolTip.text: card.modelData.account
                                        MouseArea {
                                            id: accountHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            acceptedButtons: Qt.NoButton
                                        }
                                    }
                                    Label {
                                        visible: !!card.modelData.account
                                        text: qsTr("Codex")
                                        color: win.muted
                                        font.pixelSize: 12
                                    }
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
                                            readonly property color fill: modelData.left <= 10 ? win.bad : win.providerColor(card.provider)
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
                                    }
                                    Label {
                                        text: backend.metric === "tokens" ? win.capitalize(win.rangeText(backend.windowDays))
                                            : qsTr("%1. Not the subscription bill.").arg(win.capitalize(win.rangeText(backend.windowDays)))
                                        color: win.faint
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        visible: backend.usageSource.length > 0
                                        text: win.sourceText(backend.usageSource)
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

                            // Same chart as T3 Code's usage page: one smoothed area per provider,
                            // layered rather than stacked, drawn in a 960 x 260 view box.
                            ColumnLayout {
                                visible: backend.buckets.length > 0
                                Layout.fillWidth: true
                                spacing: 12

                                Label {
                                    text: chart.hourly
                                          ? (backend.metric === "tokens" ? qsTr("Hourly processed tokens") : qsTr("Hourly cost"))
                                          : (backend.metric === "tokens" ? qsTr("Daily processed tokens") : qsTr("Daily cost"))
                                    color: win.ink
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                }

                                Item {
                                    id: chart
                                    objectName: "usageChart"
                                    Layout.fillWidth: true
                                    implicitHeight: plotArea.height + 4 + axisRow.height
                                    readonly property int count: backend.buckets.length
                                    readonly property bool hourly: count > 0 && backend.buckets[0].day === undefined
                                    readonly property real viewWidth: 960
                                    readonly property real viewHeight: 260
                                    readonly property real viewTop: 8
                                    property int hovered: -1
                                    property real hoverX: 0
                                    property real hoverY: 0

                                    readonly property var series: {
                                        var values = {}
                                        var totals = {}
                                        var peak = 0
                                        for (var i = 0; i < count; ++i) {
                                            var parts = backend.buckets[i].parts
                                            for (var j = 0; j < parts.length; ++j) {
                                                var id = parts[j].provider
                                                if (!values[id]) {
                                                    values[id] = Array.from({ length: count }, function () { return 0 })
                                                    totals[id] = 0
                                                }
                                                values[id][i] += parts[j].value
                                                totals[id] += parts[j].value
                                                peak = Math.max(peak, values[id][i])
                                            }
                                        }
                                        var providers = win.chartOrder.filter(function (id) { return totals[id] > 0 })
                                        for (var key in totals)
                                            if (totals[key] > 0 && providers.indexOf(key) < 0)
                                                providers.push(key)
                                        // The largest series paints first, so smaller ones stay visible on top.
                                        var paint = providers.slice().sort(function (a, b) { return totals[b] - totals[a] })
                                        return { values: values, providers: providers, paint: paint, scale: win.niceScale(peak, 4) }
                                    }

                                    function viewY(value) {
                                        var max = series.scale.max
                                        return max === 0 ? viewHeight : viewHeight - value / max * (viewHeight - viewTop)
                                    }

                                    function columnTotal(index) {
                                        var sum = 0
                                        for (var i = 0; i < series.providers.length; ++i)
                                            sum += series.values[series.providers[i]][index]
                                        return sum
                                    }

                                    onSeriesChanged: canvas.requestPaint()

                                    Item {
                                        id: tickColumn
                                        width: 56
                                        height: plotArea.height
                                        Repeater {
                                            model: chart.series.scale.ticks
                                            delegate: Label {
                                                required property real modelData
                                                anchors.right: parent.right
                                                y: chart.viewY(modelData) / chart.viewHeight * tickColumn.height - height / 2
                                                text: modelData === 0 ? "0" : win.chartMetric(modelData)
                                                color: win.muted
                                                font.pixelSize: 10
                                            }
                                        }
                                    }

                                    Item {
                                        id: plotArea
                                        x: tickColumn.width + 8
                                        width: chart.width - x
                                        height: 224
                                        readonly property real stepX: chart.count < 2 ? 0 : width / (chart.count - 1)

                                        Canvas {
                                            id: canvas
                                            anchors.fill: parent
                                            onWidthChanged: requestPaint()
                                            onHeightChanged: requestPaint()

                                            onPaint: {
                                                var ctx = getContext("2d")
                                                ctx.reset()
                                                var sx = width / chart.viewWidth
                                                var sy = height / chart.viewHeight
                                                var ticks = chart.series.scale.ticks
                                                ctx.lineWidth = 1
                                                ctx.strokeStyle = "rgba(255, 255, 255, 0.08)"
                                                for (var t = 0; t < ticks.length; ++t) {
                                                    var gy = Math.min(height - 0.5, Math.max(0.5, Math.round(chart.viewY(ticks[t]) * sy) + 0.5))
                                                    ctx.beginPath()
                                                    ctx.moveTo(0, gy)
                                                    ctx.lineTo(width, gy)
                                                    ctx.stroke()
                                                }
                                                if (chart.count < 2)
                                                    return

                                                var stepX = chart.viewWidth / (chart.count - 1)
                                                var curves = chart.series.paint.map(function (id) {
                                                    var points = chart.series.values[id].map(function (v, i) {
                                                        return { x: i * stepX, y: chart.viewY(v) }
                                                    })
                                                    return { color: win.providerColor(id), segments: win.smoothCurve(points) }
                                                })
                                                function trace(segments) {
                                                    ctx.moveTo(segments[0].from.x * sx, segments[0].from.y * sy)
                                                    for (var s = 0; s < segments.length; ++s) {
                                                        var seg = segments[s]
                                                        ctx.bezierCurveTo(seg.c1.x * sx, seg.c1.y * sy, seg.c2.x * sx, seg.c2.y * sy,
                                                                          seg.to.x * sx, seg.to.y * sy)
                                                    }
                                                }

                                                ctx.globalAlpha = 0.12
                                                for (var a = 0; a < curves.length; ++a) {
                                                    ctx.beginPath()
                                                    trace(curves[a].segments)
                                                    ctx.lineTo(width, height)
                                                    ctx.lineTo(0, height)
                                                    ctx.closePath()
                                                    ctx.fillStyle = curves[a].color
                                                    ctx.fill()
                                                }
                                                ctx.globalAlpha = 1
                                                ctx.lineWidth = 2
                                                ctx.lineJoin = "round"
                                                for (var l = 0; l < curves.length; ++l) {
                                                    ctx.beginPath()
                                                    trace(curves[l].segments)
                                                    ctx.strokeStyle = curves[l].color
                                                    ctx.stroke()
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: chart.hovered >= 0
                                            x: Math.round(chart.hovered * plotArea.stepX)
                                            y: chart.viewTop / chart.viewHeight * plotArea.height
                                            width: 1
                                            height: plotArea.height - y
                                            color: win.muted
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onPositionChanged: function (mouse) {
                                                if (chart.count === 0)
                                                    return
                                                chart.hoverX = Math.max(0, Math.min(width, mouse.x))
                                                chart.hoverY = Math.max(0, Math.min(height, mouse.y))
                                                chart.hovered = Math.max(0, Math.min(chart.count - 1,
                                                                                     Math.round(chart.hoverX / width * (chart.count - 1))))
                                            }
                                            onExited: chart.hovered = -1
                                        }

                                        Rectangle {
                                            id: tip
                                            readonly property int index: chart.hovered
                                            readonly property real gap: 12
                                            visible: index >= 0 && index < chart.count
                                            width: Math.max(144, tipBody.implicitWidth + 20)
                                            height: tipBody.implicitHeight + 16
                                            x: {
                                                var left = chart.hoverX + gap + width <= plotArea.width ? chart.hoverX + gap : chart.hoverX - gap - width
                                                return Math.min(Math.max(0, left), Math.max(0, plotArea.width - width))
                                            }
                                            y: {
                                                var top = chart.hoverY + gap + height <= plotArea.height ? chart.hoverY + gap : chart.hoverY - gap - height
                                                return Math.min(Math.max(0, top), Math.max(0, plotArea.height - height))
                                            }
                                            radius: 12
                                            color: Qt.rgba(win.bg.r, win.bg.g, win.bg.b, 0.9)
                                            border.color: Qt.rgba(1, 1, 1, 0.04)

                                            ColumnLayout {
                                                id: tipBody
                                                x: 10
                                                y: 8
                                                width: tip.width - 20
                                                spacing: 0
                                                Label {
                                                    Layout.bottomMargin: 4
                                                    text: tip.visible ? win.chartTooltipPeriod(backend.buckets[tip.index]) : ""
                                                    color: win.muted
                                                    font.pixelSize: 12
                                                }
                                                Repeater {
                                                    model: chart.series.providers
                                                    delegate: RowLayout {
                                                        required property string modelData
                                                        Layout.fillWidth: true
                                                        spacing: 12
                                                        RowLayout {
                                                            spacing: 6
                                                            Rectangle {
                                                                implicitWidth: 8
                                                                implicitHeight: 8
                                                                radius: 4
                                                                color: win.providerColor(modelData)
                                                            }
                                                            Label {
                                                                text: win.providerLabel(modelData)
                                                                color: win.muted
                                                                font.pixelSize: 12
                                                            }
                                                        }
                                                        Label {
                                                            Layout.fillWidth: true
                                                            horizontalAlignment: Text.AlignRight
                                                            text: tip.visible ? win.chartMetric(chart.series.values[modelData][tip.index]) : ""
                                                            color: win.ink
                                                            font.pixelSize: 12
                                                        }
                                                    }
                                                }
                                                Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.topMargin: 4
                                                    implicitHeight: 1
                                                    color: win.chartGrid
                                                }
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Layout.topMargin: 4
                                                    spacing: 12
                                                    Label {
                                                        text: qsTr("Total")
                                                        color: win.muted
                                                        font.pixelSize: 12
                                                    }
                                                    Label {
                                                        Layout.fillWidth: true
                                                        horizontalAlignment: Text.AlignRight
                                                        text: tip.visible ? win.chartMetric(chart.columnTotal(tip.index)) : ""
                                                        color: win.ink
                                                        font.pixelSize: 12
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // First, middle, and last period, spread like CSS justify-between.
                                    Item {
                                        id: axisRow
                                        x: 64
                                        y: plotArea.height + 4
                                        width: chart.width - x
                                        height: firstTick.implicitHeight
                                        Label {
                                            id: firstTick
                                            text: win.chartPeriod(backend.buckets[0])
                                            color: win.muted
                                            font.pixelSize: 10
                                            font.capitalization: Font.AllUppercase
                                        }
                                        Label {
                                            x: firstTick.width + (lastTick.x - firstTick.width - width) / 2
                                            text: win.chartPeriod(backend.buckets[Math.floor(chart.count / 2)])
                                            color: win.muted
                                            font.pixelSize: 10
                                            font.capitalization: Font.AllUppercase
                                        }
                                        Label {
                                            id: lastTick
                                            anchors.right: parent.right
                                            text: win.chartPeriod(backend.buckets[chart.count - 1])
                                            color: win.muted
                                            font.pixelSize: 10
                                            font.capitalization: Font.AllUppercase
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
                                                    Layout.alignment: Qt.AlignBaseline
                                                }
                                                Label {
                                                    text: win.formatPercent(modelData.share * 100)
                                                    color: win.faint
                                                    font.pixelSize: 12
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
                                                horizontalAlignment: Text.AlignRight
                                                Layout.preferredWidth: 80
                                            }
                                            Label {
                                                text: win.formatUsd(modelData.cost)
                                                color: backend.metric === "cost" ? win.ink : win.muted
                                                font.pixelSize: 13
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
