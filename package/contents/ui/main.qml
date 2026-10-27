import QtQuick
import org.kde.kwin
import com.github.luisbocanegra.trayicon 1.0
import "../code/utils.js" as Utils

Item {
    id: root
    property bool debugEnabled: false
    property bool countUseDot: false
    property int countDisplayLimit: 99
    property bool hideOnMinimize: false
    property bool shuttingDown: false
    property var workspaceConnections: []
    property var trayIcons: new Object()
    property var automaticMatches: new Object()
    property var automaticPins: new Object()
    property var automaticDecisions: new Object()
    property var hiddenWindows: new Object()
    property var trackedWindows: new Object()
    property var pendingWindows: new Object()
    property var pendingActivation: null
    readonly property Component trayIconComponent: TrayIcon {
        countUseDot: root.countUseDot
        countDisplayLimit: root.countDisplayLimit
    }

    ScriptConfig {
        id: scriptConfig
        onValuesChanged: root.reloadConfig()
        onWindowsRequested: scriptConfig.setWindows(Workspace.windows.map(window => Utils.windowInfo(window)))
    }

    function reloadConfig() {
        const config = scriptConfig.values;
        debugEnabled = config.debugEnabled;
        countUseDot = config.countUseDot;
        countDisplayLimit = config.countDisplayLimit;
        hideOnMinimize = config.hideOnMinimize;
        Workspace.windows.forEach(window => Utils.scheduleWindow(window));
    }

    Timer {
        id: reconcileTimer
        interval: 0
        onTriggered: Utils.reconcileWindows()
    }

    ShortcutHandler {
        name: "Minimize to tray"
        text: "Minimize window to tray (sets skip Taskbar, Switcher & Pager)"
        sequence: "Meta+Alt+PgDown"
        onActivated: {
            let window = Workspace.activeWindow;
            if (!window || Utils.isProtectedWindow(window)) return;
            while (window && window.transient) {
                window = window.transientFor;
            }
            if (!Utils.isValidWindow(window))
                return;
            if (Utils.addTrayIcon(window)) Utils.toggleShowHide(window.internalId);
        }
    }

    ShortcutHandler {
        name: "Close or hide window to tray"
        text: "Close or hide window to tray (according to window rules)"
        sequence: ""
        onActivated: Utils.closeOrHideWindow(Workspace.activeWindow)
    }

    Component.onCompleted: {
        Utils.connectSignal(workspaceConnections, Workspace.currentActivityChanged, () => Utils.finishWindowActivation());
        Utils.connectSignal(workspaceConnections, Workspace.windowAdded, window => {
            Utils.setup(window);
            Utils.scheduleWindow(window);
        });
        Utils.connectSignal(workspaceConnections, Workspace.windowRemoved, window => Utils.forgetWindow(window));
        Workspace.windows.forEach(window => Utils.setup(window));
        reloadConfig();
    }

    Component.onDestruction: {
        Utils.restoreManagedWindows();
    }
}
