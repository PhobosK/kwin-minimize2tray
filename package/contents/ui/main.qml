import QtQuick
import org.kde.kwin
import com.github.luisbocanegra.trayicon 1.0
import "../code/utils.js" as Utils

Item {
    id: root
    property bool debugEnabled: false
    property var hideByDefaultClass: []
    property bool countUseDot: false
    property int countDisplayLimit: 99
    property bool hideOnMinimize: false
    property var trayIcons: new Object()
    readonly property Component trayIconComponent: TrayIcon {
        countUseDot: root.countUseDot
        countDisplayLimit: root.countDisplayLimit
    }

    ScriptConfig {
        id: scriptConfig
        onValuesChanged: root.reloadConfig()
    }

    function reloadConfig() {
        const previousClasses = hideByDefaultClass;
        const config = scriptConfig.values;
        debugEnabled = config.debugEnabled;
        hideByDefaultClass = Utils.commaSeparate(config.hideByDefaultClass);
        countUseDot = config.countUseDot;
        countDisplayLimit = config.countDisplayLimit;
        hideOnMinimize = config.hideOnMinimize;

        // Only new matches should hide windows the user may have restored.
        Workspace.windows.forEach(window => {
            const wasMatched = Utils.matchesAutoHide(window, previousClasses);
            const isMatched = Utils.matchesAutoHide(window, hideByDefaultClass);
            if (wasMatched && !isMatched && window.internalId in trayIcons) {
                // Drop ownership before the skip-state guard can run.
                Utils.removeTrayIcon(window);
                Utils.setSkip(false, window);
                Utils.setMinimize(false, window);
            } else if (!wasMatched && isMatched) {
                Utils.setupAutoHide(window);
            }
        });
    }

    ShortcutHandler {
        name: "Minimize to tray"
        text: "Minimize window to tray (sets skip Taskbar, Switcher & Pager)"
        sequence: "Meta+Alt+PgDown"
        onActivated: {
            let window = Workspace.activeWindow;
            while (window.transient) {
                window = window.transientFor;
            }
            if (!Utils.isValidWindow(window))
                return;
            Utils.toggleShowHide(window.internalId);
            Utils.addTrayIcon(window);
        }
    }

    Component.onCompleted: {
        Workspace.windowAdded.connect(window => Utils.setup(window));
        Workspace.windowAdded.connect(window => {
            Utils.setupAutoHide(window);
        });
        Workspace.windowRemoved.connect(window => Utils.removeTrayIcon(window));
        Workspace.windows.forEach(window => Utils.setup(window));
        reloadConfig();
    }

    Component.onDestruction: {
        Utils.restoreManagedWindows();
    }
}
