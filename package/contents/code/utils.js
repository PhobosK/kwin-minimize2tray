function commaSeparate(string) {
    if (!string || typeof string !== "string") {
        return [];
    }
    return string
        .split(",")
        .map((part) => part.trim())
        .filter((part) => part != "");
}

function isValidWindow(window) {
    return window && !isProtectedWindow(window) && window.minimizable && window.normalWindow &&
        !window.dialog && !window.modal && !window.specialWindow &&
        !window.appletPopup && !window.splash && !window.utility && !window.transient;
}

function dumpProps(obj) {
    for (var k of Object.keys(obj)) {
        const val = obj[k];
        if (k === 'metaData')
            continue;
        print(k + "=" + val + "\n");
    }
}

function connectSignal(connections, signal, callback) {
    const connection = {signal: signal, callback: null, active: true};
    connection.callback = (...args) => {
        if (connection.active && !shuttingDown) callback(...args);
    };
    signal.connect(connection.callback);
    connections.push(connection);
}

function disconnectSignals(connections) {
    connections.forEach(connection => {
        // Invalidate already queued calls too; disconnect alone cannot cancel them.
        connection.active = false;
        // A window can already have been destroyed during shutdown.
        try { connection.signal.disconnect(connection.callback); } catch (error) {}
    });
}

function setup(window) {
    if (shuttingDown || window.internalId in trackedWindows) return;
    const connections = [];
    trackedWindows[window.internalId] = connections;
    const watch = (signal, callback) => connectSignal(connections, signal, callback);
    watch(window.captionChanged, () => {
        updateToolTip(window);
        scheduleWindow(window);
    });
    watch(window.windowClassChanged, () => scheduleWindow(window));
    watch(window.windowRoleChanged, () => scheduleWindow(window));
    watch(window.transientChanged, () => scheduleWindow(window));
    watch(window.modalChanged, () => scheduleWindow(window));
    watch(window.minimizeableChanged, () => scheduleWindow(window));
    watch(window.desktopsChanged, () => updateToolTip(window));
    watch(window.outputChanged, () => updateToolTip(window));
    watch(window.demandsAttentionChanged, () => {
        updateDemandsAttention(window);
        enforceTrayManagedState(window);
    });
    watch(window.minimizedChanged, () => onMinimizeChanged(window));
    watch(window.windowShown, () => {
        if (!window.minimized) onMinimizeChanged(window);
    });
    watch(window.skipTaskbarChanged, () => enforceTrayManagedState(window));
    watch(window.skipPagerChanged, () => enforceTrayManagedState(window));
    watch(window.skipSwitcherChanged, () => enforceTrayManagedState(window));
    watch(window.iconChanged, () => updateWindowIcon(window));
    watch(window.desktopFileNameChanged, () => {
        updateWindowIcon(window);
        scheduleWindow(window);
    });
}

function forgetWindow(window) {
    const windowId = window.internalId;
    if (pendingActivation === windowId) pendingActivation = null;
    if (windowId in trackedWindows) disconnectSignals(trackedWindows[windowId]);
    removeTrayIcon(window);
    delete automaticMatches[windowId];
    delete automaticDecisions[windowId];
    delete hiddenWindows[windowId];
    delete trackedWindows[windowId];
    delete pendingWindows[windowId];
}

function setupTrayicon(window) {
    if (window.skipTaskbar) {
        addTrayIcon(window);
    } else {
        removeTrayIcon(window);
    }
}

function formattedWindowInfo(w) {
    let desktops;
    if (w.onAllDesktops) {
        desktops = "All Desktops";
    } else {
        desktops = w.desktops.map(desktop => desktop.name).join(", ");
    }
    return `${w.caption || w.resourceName || w.resourceClass}\n${desktops} @ ${w.output.name}`;
}

function addTrayIcon(window, automatic = false) {
    if (shuttingDown || isProtectedWindow(window)) return false;
    if (!automatic) {
        cancelStartupProtection(window.internalId);
        // An explicit shortcut pin takes ownership independently of the rules.
        delete automaticPins[window.internalId];
        if (automaticDecisions[window.internalId] === "unpinned") automaticDecisions[window.internalId] = "shown";
        if (window.internalId in hiddenWindows) hiddenWindows[window.internalId].automatic = false;
    }
    const windowId = window.internalId;
    if (windowId in trayIcons) {
        return true;
    }
    const trayItem = trayIconComponent.createObject(root, {
        "icon": window.icon,
        "windowId": windowId,
        "toolTipText": formattedWindowInfo(window),
        "xdgName": window.desktopFileName,
        "demandsAttention": window.demandsAttention
    });
    if (!trayItem) return false;
    trayItem.requestShowHide.connect(toggleShowHide);
    trayItem.requestClose.connect(closeWindow);
    trayItem.requestUnpin.connect(unpinIcon);
    trayItem.requestAbout.connect(() => scriptConfig.showAbout());
    trayItem.requestConfigure.connect(() => scriptConfig.configure());
    trayIcons[windowId] = trayItem;
    if (automatic) automaticPins[windowId] = true;
    return true;
}

function removeTrayIcon(window) {
    const windowId = window.internalId;
    cancelStartupProtection(windowId);
    if (!(windowId in trayIcons)) {
        return;
    }
    const trayItem = trayIcons[windowId];
    delete trayIcons[windowId];
    delete automaticPins[windowId];
    trayItem.destroy();
}

function restoreManagedWindows() {
    shuttingDown = true;
    cancelAllStartupProtection();
    reconcileTimer.stop();
    pendingWindows = new Object();
    disconnectSignals(workspaceConnections);
    Object.keys(trackedWindows).forEach(id => disconnectSignals(trackedWindows[id]));
    trackedWindows = new Object();
    pendingActivation = null;
    Workspace.windows.forEach(window => {
        // Drop tray ownership before restoring flags so the guard cannot rehide it.
        delete trayIcons[window.internalId];
        releaseHiddenWindow(window, true);
    });
    trayIcons = new Object();
    automaticPins = new Object();
    hiddenWindows = new Object();
    automaticDecisions = new Object();
}

function cancelStartupProtection(windowId, expected) {
    const protection = startupProtections[windowId];
    if (!protection || expected && protection !== expected) return false;
    delete startupProtections[windowId];
    protection.timer.stop();
    protection.timer.destroy();
    const window = getWindow(protection.windowId);
    const hidden = hiddenWindows[windowId];
    if (protection.pending && window && !window.minimized && hidden && hidden.automatic && hidden.reason === "startup") {
        // A deferred retry temporarily retains skip flags; cancellation must release them.
        automaticDecisions[windowId] = "shown";
        releaseHiddenWindow(window, false);
    }
    return protection.pending;
}

function cancelAllStartupProtection() {
    Object.keys(startupProtections).forEach(id => cancelStartupProtection(id));
}

function activeWindowBelongsTo(window) {
    const visited = new Set();
    for (let active = Workspace.activeWindow; active && !visited.has(active); active = active.transientFor) {
        if (active === window) return true;
        visited.add(active);
    }
    return false;
}

function cancelActiveStartupProtection() {
    const visited = new Set();
    for (let active = Workspace.activeWindow; active && !visited.has(active); active = active.transientFor) {
        visited.add(active);
        cancelStartupProtection(active.internalId);
    }
}

function armStartupProtection(window) {
    const windowId = window.internalId;
    cancelStartupProtection(windowId);
    const protection = {windowId: windowId, window: window, identity: JSON.stringify(windowInfo(window)),
                        started: scriptConfig.monotonicMilliseconds(), pending: false};
    const timer = startupProtectionComponent.createObject(root);
    if (!timer) return;
    protection.timer = timer;
    startupProtections[windowId] = protection;
    // Capture the token directly; createObject's initial properties can copy JS objects.
    timer.triggered.connect(() => cancelStartupProtection(windowId, protection));
    timer.start();
}

function startupProtectionApplies(window, protection) {
    const windowId = window.internalId;
    const hidden = hiddenWindows[windowId];
    const age = scriptConfig.monotonicMilliseconds() - protection.started;
    if (shuttingDown || startupProtections[windowId] !== protection || getWindow(windowId) !== window ||
        !(windowId in trackedWindows) || !(windowId in automaticPins) || !(windowId in trayIcons) ||
        !hidden || !hidden.automatic || hidden.reason !== "startup" || age < 0 || age >= 1500 ||
        automaticDecisions[windowId] === "shown" || automaticDecisions[windowId] === "unpinned" ||
        activeWindowBelongsTo(window) || JSON.stringify(windowInfo(window)) !== protection.identity) return false;
    const behavior = scriptConfig.matchingBehavior(windowInfo(window));
    return behavior.matched && behavior.startInTray && behavior.startupProtection;
}

function queueStartupRetry(window) {
    const windowId = window.internalId;
    const protection = startupProtections[windowId];
    if (!protection) return false;
    if (!startupProtectionApplies(window, protection)) {
        cancelStartupProtection(windowId, protection);
        return false;
    }
    if (!protection.pending) {
        protection.pending = true;
        // Let activation and explicit commands win before considering a retry.
        // Use a distinct closure per window: callLater coalesces identical functions.
        Qt.callLater(() => retryStartupProtection(windowId, protection));
    }
    return true;
}

function retryStartupProtection(windowId, protection) {
    if (startupProtections[windowId] !== protection) return;
    const window = getWindow(windowId);
    if (!window || window !== protection.window || window.minimized || !startupProtectionApplies(window, protection)) {
        cancelStartupProtection(windowId, protection);
        return;
    }
    // Consume the sole retry before writing window state, including reentrant signals.
    protection.pending = false;
    cancelStartupProtection(windowId, protection);
    hideWindow(window, true, "startup");
}

function hideWindow(window, automatic = false, reason = "manual") {
    const windowId = window.internalId;
    if (reason !== "startup") cancelStartupProtection(windowId);
    if (shuttingDown || !(windowId in trayIcons) ||
        (!isValidWindow(window) && !scriptConfig.matchingBehavior(windowInfo(window)).matched)) return false;
    if (pendingActivation === windowId) pendingActivation = null;
    if (!(windowId in hiddenWindows)) {
        hiddenWindows[windowId] = {
            automatic: automatic,
            reason: reason,
            skipTaskbar: window.skipTaskbar,
            skipPager: window.skipPager,
            skipSwitcher: window.skipSwitcher
        };
    } else if (!automatic) {
        hiddenWindows[windowId].automatic = false;
        hiddenWindows[windowId].reason = reason;
    }
    window.minimized = true;
    enforceTrayManagedState(window);
    return true;
}

function releaseHiddenWindow(window, restore) {
    const windowId = window.internalId;
    cancelStartupProtection(windowId);
    const hidden = hiddenWindows[windowId];
    if (!hidden) return;
    delete hiddenWindows[windowId];
    window.skipTaskbar = hidden.skipTaskbar;
    window.skipPager = hidden.skipPager;
    window.skipSwitcher = hidden.skipSwitcher;
    if (restore) window.minimized = false;
}

function setMinimize(minimize, window) {
    cancelStartupProtection(window.internalId);
    window.minimized = minimize;
}

function setSkip(skip, window) {
    window.skipPager = skip;
    window.skipTaskbar = skip;
    window.skipSwitcher = skip;
}

function enforceTrayManagedState(window) {
    if (!(window.internalId in trayIcons) || !(window.internalId in hiddenWindows) || !window.minimized) {
        return;
    }

    /*
     * A tray-managed minimized window normally stays out of Plasma Task
     * Manager. Keep the existing attention behaviour by exposing it there
     * only while KWin reports that it demands attention.
     */
    const skipTaskbar = !window.demandsAttention;

    if (window.skipTaskbar !== skipTaskbar) {
        window.skipTaskbar = skipTaskbar;
    }
    if (!window.skipPager) {
        window.skipPager = true;
    }
    if (!window.skipSwitcher) {
        window.skipSwitcher = true;
    }
}

function toggleMinimize(windowId) {
    const window = getWindow(windowId);
    if (!window) return;
    cancelStartupProtection(windowId);
    window.minimized = !window.minimized;
}

function toggleShowHide(windowId, forceShow = false) {
    const window = getWindow(windowId);
    if (!window) return;

    const restoring = cancelStartupProtection(windowId);
    if (window.minimized || restoring || forceShow) {
        automaticDecisions[windowId] = "shown";
        releaseHiddenWindow(window, true);
        window.minimized = false;
        activateWindowInPlace(window);
    } else {
        hideWindow(window);
    }
}

function activateWindowInPlace(window) {
    cancelStartupProtection(window.internalId);
    pendingActivation = window.internalId;
    const activities = window.activities;
    if (activities.length && activities.indexOf(Workspace.currentActivity) === -1) {
        // Activity changes are asynchronous; focus only after KWin reports the new activity.
        Workspace.currentActivity = activities[0];
        return;
    }
    finishWindowActivation();
}

function finishWindowActivation() {
    if (pendingActivation === null) return;
    const window = getWindow(pendingActivation);
    if (!window || window.minimized || isProtectedWindow(window)) {
        pendingActivation = null;
        return;
    }
    if (window.activities.length && window.activities.indexOf(Workspace.currentActivity) === -1) return;
    pendingActivation = null;

    const desktops = window.desktops;
    const perScreen = window.output && typeof Workspace.currentDesktopForScreen === "function" &&
        typeof Workspace.setCurrentDesktopForScreen === "function";
    const current = perScreen ? Workspace.currentDesktopForScreen(window.output) : Workspace.currentDesktop;
    if (desktops.length && desktops.indexOf(current) === -1) {
        // Switch the view first so KWin's activation policy cannot bring the window here.
        const desktop = desktops[desktops.length - 1];
        if (perScreen) Workspace.setCurrentDesktopForScreen(desktop, window.output);
        else Workspace.currentDesktop = desktop;
    }
    Workspace.activeWindow = window;
}

function onMinimizeChanged(window) {
    const windowId = window.internalId;
    if (window.minimized && pendingActivation === windowId) pendingActivation = null;
    if (!window.minimized) {
        if (windowId in hiddenWindows) {
            if (queueStartupRetry(window)) return;
            // External restores have no reliable user/application attribution.
            automaticDecisions[windowId] = "shown";
            releaseHiddenWindow(window, false);
        }
    } else if (windowId in hiddenWindows) {
        enforceTrayManagedState(window);
    } else {
        const behavior = scriptConfig.matchingBehavior(windowInfo(window));
        if (behavior.minimizeToTray && automaticDecisions[windowId] !== "unpinned" &&
            (behavior.matched || windowId in trayIcons && isValidWindow(window))) {
            if (!(windowId in trayIcons) && !addTrayIcon(window, true)) return;
            hideWindow(window, true, "minimize");
        }
    }
}

// This is an opt-in close path, not interception of KWin or application close requests.
function closeOrHideWindow(window) {
    if (shuttingDown || !window || isProtectedWindow(window) || !getWindow(window.internalId)) return;
    const windowId = window.internalId;
    const behavior = scriptConfig.matchingBehavior(windowInfo(window));
    if (behavior.matched && behavior.closeToTray && automaticDecisions[windowId] !== "unpinned") {
        if (!(windowId in trayIcons) && !addTrayIcon(window, true)) return;
        hideWindow(window, true, "close");
    } else if (window.closeable) {
        closeWindow(windowId);
    }
}

function getWindow(windowId) {
    return Workspace.windows.find(w => w.internalId === windowId);
}

function closeWindow(windowId) {
    const window = getWindow(windowId);
    if (!window) return;

    if (pendingActivation === windowId) pendingActivation = null;
    cancelStartupProtection(windowId);
    window.closeWindow();
}

function unpinIcon(windowId) {
    const window = getWindow(windowId);
    if (!window) return;
    cancelStartupProtection(windowId);
    automaticDecisions[windowId] = "unpinned";
    removeTrayIcon(window);
    releaseHiddenWindow(window, true);
    window.minimized = false;
    activateWindowInPlace(window);
}

function updateToolTip(window) {
    if (window.internalId in trayIcons) {
        trayIcons[window.internalId].toolTipText = formattedWindowInfo(window);
    }
}

function updateWindowIcon(window) {
    if (window.internalId in trayIcons) {
        trayIcons[window.internalId].xdgName = window.desktopFileName;
        trayIcons[window.internalId].icon = window.icon;
    }
}

function updateDemandsAttention(window) {
    if (window.internalId in trayIcons) {
        trayIcons[window.internalId].demandsAttention = window.demandsAttention;
    }
}

function windowInfo(window) {
    return {
        uuid: window.internalId.toString(),
        resourceClass: window.resourceClass,
        resourceName: window.resourceName,
        desktopFile: window.desktopFileName,
        role: window.windowRole || "",
        caption: window.caption,
        type: window.normalWindow ? "normal" : window.dialog ? "dialog" : window.utility ? "utility" : "other",
        hasTransientParent: window.transient,
        modal: window.modal,
        specialWindow: window.specialWindow || window.appletPopup || window.splash,
        minimizable: window.minimizable
    };
}

function isProtectedWindow(window) {
    return window && scriptConfig.isProtected(windowInfo(window));
}

function scheduleWindow(window) {
    if (shuttingDown) return;
    cancelStartupProtection(window.internalId);
    pendingWindows[window.internalId] = true;
    reconcileTimer.restart();
}

function reconcileWindows() {
    if (shuttingDown) return;
    const pending = pendingWindows;
    pendingWindows = new Object();
    Workspace.windows.forEach(window => {
        if (!(window.internalId in pending)) return;
        const windowId = window.internalId;
        const previous = automaticMatches[windowId] || {matched: false, startInTray: false};
        const behavior = scriptConfig.matchingBehavior(windowInfo(window));
        automaticMatches[windowId] = behavior;
        if (isProtectedWindow(window) || (!behavior.matched && !isValidWindow(window))) {
            removeTrayIcon(window);
            releaseHiddenWindow(window, true);
            delete automaticDecisions[windowId];
            return;
        }
        let hidden = hiddenWindows[windowId];
        if (!behavior.matched) {
            if (windowId in automaticPins) {
                removeTrayIcon(window);
                releaseHiddenWindow(window, true);
            } else if (hidden && hidden.automatic) {
                if (hidden.reason === "minimize" && !behavior.minimizeToTray) releaseHiddenWindow(window, false);
                else if (hidden.reason === "close") releaseHiddenWindow(window, true);
            }
            delete automaticDecisions[windowId];
            return;
        }
        if (automaticDecisions[windowId] === "unpinned") return;
        // Disabling an automatic action releases only the state that action owns.
        if (hidden && hidden.automatic &&
            (hidden.reason === "startup" && !behavior.startInTray ||
             hidden.reason === "minimize" && !behavior.minimizeToTray ||
             hidden.reason === "close" && !behavior.closeToTray)) {
            releaseHiddenWindow(window, hidden.reason !== "minimize");
        }
        if (!(behavior.autoPin || behavior.startInTray || behavior.minimizeToTray || behavior.closeToTray)) {
            if (windowId in automaticPins) {
                removeTrayIcon(window);
                releaseHiddenWindow(window, true);
            }
            return;
        }
        const needsIcon = !(windowId in trayIcons);
        if (needsIcon && (behavior.autoPin || behavior.startInTray || window.minimized && behavior.minimizeToTray)) {
            if (!addTrayIcon(window, true)) return;
        }
        // Startup handling never takes ownership of an independently pinned window.
        if (windowId in automaticPins && behavior.startInTray &&
            (!previous.matched || !previous.startInTray || needsIcon) &&
            automaticDecisions[windowId] !== "shown") {
            // Protection belongs only to this initial automatic hide, not later actions.
            if (behavior.startupProtection && !window.minimized) armStartupProtection(window);
            if (!hideWindow(window, true, "startup")) cancelStartupProtection(windowId);
        } else if (window.minimized && behavior.minimizeToTray && !(windowId in hiddenWindows)) {
            hideWindow(window, true, "minimize");
        }
    });
}
