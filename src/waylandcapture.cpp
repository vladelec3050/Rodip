#include "waylandcapture.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>
#include <QMetaObject>
#include <QDBusMetaType>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <cstring>

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalStream &stream) {
    argument.beginStructure();
    argument >> stream.nodeId >> stream.properties;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalStream &stream) {
    argument.beginStructure();
    argument << stream.nodeId << stream.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}

class WaylandCapture::PipewireVideo final : public QObject {
public:
    explicit PipewireVideo(WaylandCapture *owner) : owner(owner) {}

    ~PipewireVideo() override { close(); }

    bool open(int fd, const QString &target) {
        pw_init(nullptr, nullptr);
        loop = pw_thread_loop_new("rodip-screen-capture", nullptr);
        if (!loop) return false;
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (!context) return false;
        core = pw_context_connect_fd(context, fd, nullptr, 0);
        if (!core) return false;
        auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                        PW_KEY_MEDIA_CATEGORY, "Capture",
                                        PW_KEY_MEDIA_ROLE, "Screen", nullptr);
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.toUtf8().constData());
        stream = pw_stream_new(core, "rodip-screen", props);
        if (!stream) return false;
        static const pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.param_changed = [](void *data, uint32_t id, const spa_pod *param) {
                if (id != SPA_PARAM_Format || !param) return;
                auto *self = static_cast<PipewireVideo *>(data);
                spa_video_info_raw info{};
                if (spa_format_video_raw_parse(param, &info) < 0 || info.format != SPA_VIDEO_FORMAT_BGRx) {
                    QMetaObject::invokeMethod(self->owner, [owner = self->owner] { emit owner->failed("Compositor не предоставил поддерживаемый BGRx видеопоток."); }, Qt::QueuedConnection);
                    return;
                }
                self->width = static_cast<int>(info.size.width);
                self->height = static_cast<int>(info.size.height);
                self->stride = static_cast<int>(info.size.width) * 4;
                QMetaObject::invokeMethod(self->owner, [owner = self->owner, w = self->width, h = self->height] { emit owner->formatReady(w, h, "bgr0"); }, Qt::QueuedConnection);
            };
            e.process = [](void *data) {
                auto *self = static_cast<PipewireVideo *>(data);
                auto *pwBuffer = pw_stream_dequeue_buffer(self->stream);
                if (!pwBuffer) return;
                if (!pwBuffer->buffer->n_datas) {
                    pw_stream_queue_buffer(self->stream, pwBuffer);
                    return;
                }
                auto &plane = pwBuffer->buffer->datas[0];
                const int srcStride = plane.chunk && plane.chunk->stride > 0 ? plane.chunk->stride : self->stride;
                if (plane.data && plane.chunk && self->width > 0 && self->height > 0 && srcStride >= self->stride
                    && plane.chunk->size >= static_cast<uint32_t>(srcStride * self->height)
                    && plane.chunk->offset + plane.chunk->size <= plane.maxsize) {
                    QByteArray image(self->stride * self->height, Qt::Uninitialized);
                    const auto *src = static_cast<const char *>(plane.data) + plane.chunk->offset;
                    auto *dst = image.data();
                    for (int row = 0; row < self->height; ++row) std::memcpy(dst + row * self->stride, src + row * srcStride, self->stride);
                    QMetaObject::invokeMethod(self->owner, [owner = self->owner, image = std::move(image)]() mutable { emit owner->frameReady(std::move(image)); }, Qt::QueuedConnection);
                }
                pw_stream_queue_buffer(self->stream, pwBuffer);
            };
            e.state_changed = [](void *data, pw_stream_state, pw_stream_state state, const char *error) {
                if (state != PW_STREAM_STATE_ERROR) return;
                auto *self = static_cast<PipewireVideo *>(data);
                const QString reason = QString::fromUtf8(error ? error : "PipeWire screen stream failed");
                QMetaObject::invokeMethod(self->owner, [owner = self->owner, reason] { emit owner->failed(reason); }, Qt::QueuedConnection);
            };
            return e;
        }();
        pw_stream_add_listener(stream, &listener, &events, this);
        if (pw_thread_loop_start(loop) < 0) return false;
        loopStarted = true;

        uint8_t podBuffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        const spa_pod *params[1];
        spa_rectangle defaultSize{1920, 1080};
        spa_rectangle minSize{1, 1};
        spa_rectangle maxSize{8192, 8192};
        spa_fraction defaultRate{static_cast<uint32_t>(selfFrameRate()), 1};
        spa_fraction minRate{1, 1};
        spa_fraction maxRate{static_cast<uint32_t>(selfFrameRate()), 1};
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defaultRate, &minRate, &maxRate)));
        pw_thread_loop_lock(loop);
        const int result = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
        pw_thread_loop_unlock(loop);
        return result >= 0;
    }

    void close() {
        if (loop && loopStarted) pw_thread_loop_lock(loop);
        if (stream) {
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (core) { pw_core_disconnect(core); core = nullptr; }
        if (context) { pw_context_destroy(context); context = nullptr; }
        if (loop && loopStarted) pw_thread_loop_unlock(loop);
        if (loop) {
            if (loopStarted) pw_thread_loop_stop(loop);
            pw_thread_loop_destroy(loop);
            loop = nullptr;
            loopStarted = false;
        }
    }

private:
    int selfFrameRate() const { return owner->requestedFrameRate; }
    WaylandCapture *owner;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    int width = 0;
    int height = 0;
    int stride = 0;
    bool loopStarted = false;
};

WaylandCapture::WaylandCapture(QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<PortalStream>();
    qDBusRegisterMetaType<QList<PortalStream>>();
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
}

WaylandCapture::~WaylandCapture() { stop(); }

void WaylandCapture::start(int frameRate) {
    stopped = false;
    requestedFrameRate = frameRate;
    QVariantMap options;
    options.insert("handle_token", QString("rodip_%1").arg(QRandomGenerator::global()->generate()));
    options.insert("session_handle_token", QString("rodip_session_%1").arg(QRandomGenerator::global()->generate()));
    callRequest("CreateSession", {QVariant::fromValue(options)}, "create");
}

void WaylandCapture::bindSaveShortcut(const QString &preferredTrigger) {
    if (!shortcutSessionPath.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", shortcutSessionPath,
            "org.freedesktop.portal.Session", "Close");
        bus.call(close, QDBus::NoBlock);
        shortcutSessionPath.clear();
        bus.disconnect("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                       "org.freedesktop.portal.GlobalShortcuts", "Activated", this,
                       SLOT(onShortcutActivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    }
    shortcutPreferredTrigger = preferredTrigger;
    QVariantMap options;
    options.insert("handle_token", QString("rodip_shortcut_%1").arg(QRandomGenerator::global()->generate()));
    options.insert("session_handle_token", QString("rodip_shortcuts_%1").arg(QRandomGenerator::global()->generate()));
    QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "CreateSession");
    message.setArguments({QVariant::fromValue(options)});
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) { emit shortcutFailed("Не удалось создать сессию глобальной клавиши: " + reply.error().message()); return; }
        shortcutRequestPath = reply.value().path();
        if (!bus.connect("org.freedesktop.portal.Desktop", shortcutRequestPath, "org.freedesktop.portal.Request",
                         "Response", this, SLOT(onShortcutPortalResponse(uint,QVariantMap)))) {
            emit shortcutFailed("Не удалось подписаться на ответ портала глобальных клавиш.");
        }
    });
}

void WaylandCapture::onShortcutPortalResponse(uint response, const QVariantMap &results) {
    bus.disconnect("org.freedesktop.portal.Desktop", shortcutRequestPath, "org.freedesktop.portal.Request", "Response", this, SLOT(onShortcutPortalResponse(uint,QVariantMap)));
    if (response != 0) { emit shortcutFailed("Не удалось зарегистрировать глобальную горячую клавишу."); return; }
    if (shortcutSessionPath.isEmpty()) {
        shortcutSessionPath = results.value("session_handle").toString();
        QVariantMap properties;
        properties.insert("description", QString("Сохранить мгновенный повтор"));
        properties.insert("preferred_trigger", shortcutPreferredTrigger);
        QList<PortalShortcut> shortcuts{{"save-replay", properties}};
        QVariantMap options;
        options.insert("handle_token", QString("rodip_bind_%1").arg(QRandomGenerator::global()->generate()));
        QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts");
        message.setArguments({QVariant::fromValue(QDBusObjectPath(shortcutSessionPath)), QVariant::fromValue(shortcuts), QString(), QVariant::fromValue(options)});
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError()) { emit shortcutFailed("Не удалось запросить горячую клавишу у портала: " + reply.error().message()); return; }
            shortcutRequestPath = reply.value().path();
            if (!bus.connect("org.freedesktop.portal.Desktop", shortcutRequestPath, "org.freedesktop.portal.Request",
                             "Response", this, SLOT(onShortcutPortalResponse(uint,QVariantMap)))) {
                emit shortcutFailed("Не удалось подписаться на регистрацию горячей клавиши.");
            }
        });
    } else {
        if (!bus.isConnected()) return;
        bus.connect("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                    "org.freedesktop.portal.GlobalShortcuts", "Activated", this,
                    SLOT(onShortcutActivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    }
}

void WaylandCapture::onShortcutActivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong timestamp, const QVariantMap &options) {
    Q_UNUSED(timestamp);
    Q_UNUSED(options);
    if (session.path() == shortcutSessionPath && shortcutId == "save-replay") emit saveShortcutActivated();
}

void WaylandCapture::callRequest(const QString &method, const QList<QVariant> &arguments, const QString &stage) {
    QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.ScreenCast", method);
    message.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, stage] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) { fail(reply.error().message()); return; }
        connectRequestSignal(reply.value(), stage);
    });
}

void WaylandCapture::connectRequestSignal(const QDBusObjectPath &path, const QString &stage) {
    requestPath = path.path();
    currentStage = stage;
    if (!bus.connect("org.freedesktop.portal.Desktop", requestPath, "org.freedesktop.portal.Request",
                     "Response", this, SLOT(onPortalResponse(uint,QVariantMap)))) {
        fail("Не удалось подписаться на ответ ScreenCast портала.");
    }
}

void WaylandCapture::onPortalResponse(uint response, const QVariantMap &results) {
    bus.disconnect("org.freedesktop.portal.Desktop", requestPath, "org.freedesktop.portal.Request", "Response", this, SLOT(onPortalResponse(uint,QVariantMap)));
    if (stopped) return;
    if (response != 0) { fail(response == 1 ? "Пользователь отменил выбор экрана." : "Портал отказал в захвате экрана."); return; }
    if (currentStage == "create") {
        sessionPath = results.value("session_handle").value<QDBusObjectPath>().path();
        QVariantMap options;
        options.insert("handle_token", QString("rodip_select_%1").arg(QRandomGenerator::global()->generate()));
        options.insert("types", uint(1));
        options.insert("multiple", false);
        options.insert("cursor_mode", uint(2));
        callRequest("SelectSources", {QVariant::fromValue(QDBusObjectPath(sessionPath)), QVariant::fromValue(options)}, "select");
    } else if (currentStage == "select") {
        QVariantMap options;
        options.insert("handle_token", QString("rodip_start_%1").arg(QRandomGenerator::global()->generate()));
        callRequest("Start", {QVariant::fromValue(QDBusObjectPath(sessionPath)), QString(), QVariant::fromValue(options)}, "start");
    } else if (currentStage == "start") {
        openPipewire(results);
    }
}

void WaylandCapture::openPipewire(const QVariantMap &results) {
    const auto streamsArg = results.value("streams").value<QDBusArgument>();
    const auto streams = qdbus_cast<QList<PortalStream>>(streamsArg);
    if (streams.isEmpty()) { fail("ScreenCast портал не вернул видеопоток."); return; }
    const auto &stream = streams.first();
    const QString target = stream.properties.contains("pipewire-serial")
        ? stream.properties.value("pipewire-serial").toString() : QString::number(stream.nodeId);
    QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(sessionPath)), QVariantMap{}});
    const auto reply = bus.call(message);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) { fail(reply.errorMessage()); return; }
    auto remote = reply.arguments().first().value<QDBusUnixFileDescriptor>();
    const int fd = remote.takeFileDescriptor();
    video = new PipewireVideo(this);
    if (!video->open(fd, target)) { fail("Не удалось подключиться к PipeWire-потоку экрана."); return; }
}

void WaylandCapture::fail(const QString &reason) {
    if (stopped) return;
    stopped = true;
    emit failed(reason);
}

void WaylandCapture::stopCapture() {
    stopped = true;
    if (video) { delete video; video = nullptr; }
    if (!sessionPath.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", sessionPath,
            "org.freedesktop.portal.Session", "Close");
        bus.call(close, QDBus::NoBlock);
        sessionPath.clear();
    }
}

void WaylandCapture::stop() {
    stopCapture();
    if (!shortcutSessionPath.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", shortcutSessionPath,
            "org.freedesktop.portal.Session", "Close");
        bus.call(close, QDBus::NoBlock);
        shortcutSessionPath.clear();
    }
}
