#pragma once

#include <QObject>
#include <QtPlugin>

// SDK-dependent surface access stays isolated here. Process signalling and policy
// stay in the main plugin; the caller owns and must close each returned descriptor.
class SessionProcess {
  public:
    virtual ~SessionProcess() = default;
    virtual int openProcess(QObject *window) const = 0;
    virtual bool requestClose(QObject *window) const = 0;
};

#define SessionProcess_iid "com.github.luisbocanegra.minimize2tray.SessionProcess/2.0"
Q_DECLARE_INTERFACE(SessionProcess, SessionProcess_iid)
