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

function setup(window) {
    if (window.internalId in trackedWindows) return;
    trackedWindows[window.internalId] = true;
    window.captionChanged.connect(() => {
        updateToolTip(window);
        scheduleWindow(window);
    });
    window.windowClassChanged.connect(() => scheduleWindow(window));
    window.windowRoleChanged.connect(() => scheduleWindow(window));
    window.transientChanged.connect(() => scheduleWindow(window));
    window.modalChanged.connect(() => scheduleWindow(window));
    window.minimizeableChanged.connect(() => scheduleWindow(window));
    window.desktopsChanged.connect(() => {
        updateToolTip(window);
    });
    window.outputChanged.connect(() => {
        updateToolTip(window);
    });
    window.demandsAttentionChanged.connect(() => {
        updateDemandsAttention(window);
        enforceTrayManagedState(window);
    });
    window.minimizedChanged.connect(() => {
        onMinimizeChanged(window);
    });
    window.skipTaskbarChanged.connect(() => {
        enforceTrayManagedState(window);
    });
    window.skipPagerChanged.connect(() => {
        enforceTrayManagedState(window);
    });
    window.skipSwitcherChanged.connect(() => {
        enforceTrayManagedState(window);
    });
    window.iconChanged.connect(() => {
        updateWindowIcon(window);
    });
    window.desktopFileNameChanged.connect(() => {
        updateWindowIcon(window);
        scheduleWindow(window);
    });
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

function addTrayIcon(window) {
    if (isProtectedWindow(window)) return;
    const windowId = window.internalId;
    if (windowId in trayIcons) {
        return;
    }
    const trayItem = trayIconComponent.createObject(root, {
        "icon": window.icon,
        "windowId": windowId,
        "toolTipText": formattedWindowInfo(window),
        "xdgName": window.desktopFileName,
        "demandsAttention": window.demandsAttention
    });
    trayItem.requestShowHide.connect(toggleShowHide);
    trayItem.requestClose.connect(closeWindow);
    trayItem.requestUnpin.connect(unpinIcon);
    trayItem.requestAbout.connect(() => scriptConfig.showAbout());
    trayItem.requestConfigure.connect(() => scriptConfig.configure());
    trayIcons[windowId] = trayItem;
    enforceTrayManagedState(window);
}

function removeTrayIcon(window) {
    const windowId = window.internalId;
    if (!(windowId in trayIcons)) {
        return;
    }
    trayIcons[windowId].destroy();
    delete trayIcons[windowId];
}

function restoreManagedWindows() {
    pendingActivation = null;
    const managedWindowIds = Object.keys(trayIcons);

    Workspace.windows.forEach(window => {
        const windowId = window.internalId;
        if (!(windowId in trayIcons)) {
            return;
        }

        /*
         * Drop ownership before changing KWin state. The skip-state guard
         * must not reapply the hidden state while the script is unloading.
         */
        delete trayIcons[windowId];
        setSkip(false, window);
        setMinimize(false, window);
    });

    /*
     * windowRemoved normally cleans stale entries. Clear any that remain
     * defensively; their tray objects are children of root and are already
     * being destroyed with it.
     */
    managedWindowIds.forEach(windowId => {
        delete trayIcons[windowId];
    });
}

function setMinimize(minimize, window) {
    window.minimized = minimize;
}

function setSkip(skip, window) {
    window.skipPager = skip;
    window.skipTaskbar = skip;
    window.skipSwitcher = skip;
}

function enforceTrayManagedState(window) {
    if (!(window.internalId in trayIcons) || !window.minimized) {
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
    window.minimized = !window.minimized;
}

function toggleShowHide(windowId) {
    const window = getWindow(windowId);
    if (!window) return;

    window.minimized = !window.minimized;
    if (!window.minimized) activateWindowInPlace(window);

    if (window.minimized) {
        setSkip(true, window);
    } else {
        setSkip(false, window);
    }
}

function activateWindowInPlace(window) {
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
    if (window.minimized && pendingActivation === window.internalId) pendingActivation = null;
    if (!(window.internalId in trayIcons)) {
        return;
    }
    if (window.minimized && hideOnMinimize) {
        setSkip(true, window);
    }
}

function getWindow(windowId) {
    return Workspace.windows.find(w => w.internalId === windowId);
}

function closeWindow(windowId) {
    const window = getWindow(windowId);
    if (!window) return;

    window.closeWindow();
}

function unpinIcon(windowId) {
    const window = getWindow(windowId);
    if (!window) return;
    setMinimize(false, window);
    setSkip(false, window);
    removeTrayIcon(window);
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
    pendingWindows[window.internalId] = true;
    reconcileTimer.restart();
}

function reconcileWindows() {
    const pending = pendingWindows;
    pendingWindows = new Object();
    Workspace.windows.forEach(window => {
        if (!(window.internalId in pending)) return;
        const windowId = window.internalId;
        const wasMatched = automaticMatches[windowId] === true;
        const isMatched = scriptConfig.matchingRules(windowInfo(window)).length > 0;
        automaticMatches[windowId] = isMatched;
        if ((wasMatched && !isMatched || isProtectedWindow(window)) && windowId in trayIcons) {
            removeTrayIcon(window);
            setSkip(false, window);
            setMinimize(false, window);
        } else if (isMatched && !wasMatched) {
            addTrayIcon(window);
            setMinimize(true, window);
            enforceTrayManagedState(window);
        }
    });
}
