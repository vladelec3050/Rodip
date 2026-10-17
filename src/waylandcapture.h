#pragma once

#include <QObject>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QTimer>
#include <QVariantMap>

struct PortalStream {
    uint nodeId = 0;
    QVariantMap properties;
};
Q_DECLARE_METATYPE(PortalStream)

struct PortalShortcut {
    QString id;
    QVariantMap properties;
};
Q_DECLARE_METATYPE(PortalShortcut)

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalStream &stream);
QDBusArgument &operator<<(QDBusArgument &argument, const PortalStream &stream);
const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut);
QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut);

class WaylandCapture final : public QObject {
    Q_OBJECT
public:
    explicit WaylandCapture(QObject *parent = nullptr);
    ~WaylandCapture() override;
    void start(int frameRate);
    void bindSaveShortcut(const QString &preferredTrigger);
    void stop();
    void stopCapture();

signals:
    void formatReady(int width, int height, QString pixelFormat);
    void frameReady(QByteArray frame);
    void failed(QString reason);
    void shortcutFailed(QString reason);
    void saveShortcutActivated();

private slots:
    void onPortalResponse(uint response, const QVariantMap &results);
    void onShortcutPortalResponse(uint response, const QVariantMap &results);
    void onShortcutActivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong timestamp, const QVariantMap &options);

private:
    class PipewireVideo;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QTimer requestTimeout;
    QString sessionPath;
    QString requestPath;
    QString currentStage;
    QString shortcutSessionPath;
    QString shortcutRequestPath;
    QString shortcutPreferredTrigger;
    PipewireVideo *video = nullptr;
    bool stopped = false;
    int requestedFrameRate = 30;

    void callRequest(const QString &method, const QList<QVariant> &arguments, const QString &stage);
    void connectRequestSignal(const QDBusObjectPath &path, const QString &stage);
    void fail(const QString &reason);
    void openPipewire(const QVariantMap &results);
};
