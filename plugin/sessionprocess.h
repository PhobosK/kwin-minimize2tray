#pragma once

#include <QObject>
#include <QtPlugin>

// The SDK-dependent adapter only opens a kernel handle. Signalling and policy
// stay in the main plugin; the caller owns and must close the returned descriptor.
class SessionProcess {
  public:
    virtual ~SessionProcess() = default;
    virtual int openProcess(QObject *window) const = 0;
};

#define SessionProcess_iid "com.github.luisbocanegra.minimize2tray.SessionProcess/1.0"
Q_DECLARE_INTERFACE(SessionProcess, SessionProcess_iid)
