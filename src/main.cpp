#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QKeySequenceEdit>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QDateTime>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QSlider>
#include <QSocketNotifier>
#include <QStyle>
#include <QRegularExpression>
#include <QStorageInfo>
#include "waylandcapture.h"

#include <algorithm>
#include <memory>

#ifdef RODIP_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/keysymdef.h>
#endif

class QwertyKeySequenceEdit final : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;

protected:
    void focusInEvent(QFocusEvent *event) override {
        QKeySequenceEdit::focusInEvent(event);
        clear();
        if (auto *lineEdit = findChild<QLineEdit*>("qt_keysequenceedit_lineedit"))
            lineEdit->setPlaceholderText("...");
    }

    void keyPressEvent(QKeyEvent *event) override {
        const int key = qwertyKey(event->nativeScanCode());
        if (!key) {
            QKeySequenceEdit::keyPressEvent(event);
            return;
        }
        QKeyEvent mapped(event->type(), key, event->modifiers(), event->nativeScanCode(),
                         event->nativeVirtualKey(), event->nativeModifiers(), QString(),
                         event->isAutoRepeat(), event->count());
        QKeySequenceEdit::keyPressEvent(&mapped);
    }

private:
    int qwertyKey(quint32 scanCode) const {
        const QString platform = QGuiApplication::platformName();
        if (platform == "xcb" || platform.contains("wayland", Qt::CaseInsensitive))
            scanCode = scanCode >= 8 ? scanCode - 8 : 0;
        if (scanCode >= 2 && scanCode <= 11) {
            constexpr int digits[]{Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5,
                                   Qt::Key_6, Qt::Key_7, Qt::Key_8, Qt::Key_9, Qt::Key_0};
            return digits[scanCode - 2];
        }
        if (scanCode >= 59 && scanCode <= 68) return Qt::Key_F1 + scanCode - 59;
        if (scanCode == 87) return Qt::Key_F11;
        if (scanCode == 88) return Qt::Key_F12;
        if (scanCode >= 16 && scanCode <= 25) {
            constexpr int row[]{Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R, Qt::Key_T,
                                Qt::Key_Y, Qt::Key_U, Qt::Key_I, Qt::Key_O, Qt::Key_P};
            return row[scanCode - 16];
        }
        if (scanCode >= 30 && scanCode <= 38) {
            constexpr int row[]{Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F, Qt::Key_G,
                                Qt::Key_H, Qt::Key_J, Qt::Key_K, Qt::Key_L};
            return row[scanCode - 30];
        }
        if (scanCode >= 44 && scanCode <= 50) {
            constexpr int row[]{Qt::Key_Z, Qt::Key_X, Qt::Key_C, Qt::Key_V, Qt::Key_B,
                                Qt::Key_N, Qt::Key_M};
            return row[scanCode - 44];
        }
        switch (scanCode) {
        case 1: return Qt::Key_Escape;
        case 14: return Qt::Key_Backspace;
        case 15: return Qt::Key_Tab;
        case 28: return Qt::Key_Return;
        case 57: return Qt::Key_Space;
        case 102: return Qt::Key_Home;
        case 103: return Qt::Key_Up;
        case 104: return Qt::Key_PageUp;
        case 105: return Qt::Key_Left;
        case 106: return Qt::Key_Right;
        case 107: return Qt::Key_End;
        case 108: return Qt::Key_Down;
        case 109: return Qt::Key_PageDown;
        case 110: return Qt::Key_Insert;
        case 111: return Qt::Key_Delete;
        default: return 0;
        }
    }
};

class RodipWindow final : public QWidget {
    Q_OBJECT
public:
    RodipWindow() {
        setWindowTitle("Rodip — мгновенный повтор");
        setMinimumSize(620, 660);
        setWindowIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        settings = std::make_unique<QSettings>("Rodip", "Rodip");

        auto *root = new QVBoxLayout(this);
        auto *title = new QLabel("Мгновенный повтор");
        title->setObjectName("title");
        auto *subtitle = new QLabel("Запись последних секунд экрана и звука в кольцевой буфер");
        subtitle->setObjectName("subtitle");
        root->addWidget(title);
        root->addWidget(subtitle);

        auto *form = new QFormLayout;
        displayBox = new QComboBox;
        for (auto *screen : QGuiApplication::screens()) {
            const auto g = screen->geometry();
            displayBox->addItem(QString("%1 (%2×%3, %4,%5)").arg(screen->name()).arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y()), screen->name());
        }
        if (displayBox->count() == 0) displayBox->addItem("Экран не обнаружен");
        form->addRow("Экран", displayBox);

        systemAudioCheck = new QCheckBox("Записывать весь системный звук");
        microphoneBox = new QComboBox;
        populateAudioSources();
        form->addRow("Системный звук", systemAudioCheck);
        form->addRow("Микрофон", microphoneBox);

        durationSlider = new QSlider(Qt::Horizontal);
        durationSlider->setRange(5, 15 * 60);
        durationValueLabel = new QLabel;
        auto *durationRow = new QWidget;
        auto *durationLayout = new QHBoxLayout(durationRow);
        durationLayout->setContentsMargins(0, 0, 0, 0);
        durationLayout->addWidget(durationSlider, 1);
        durationLayout->addWidget(durationValueLabel);
        form->addRow("Длительность повтора", durationRow);
        fpsBox = new QComboBox;
        for (int fps : {24, 30, 60}) fpsBox->addItem(QString("%1 FPS").arg(fps), fps);
        form->addRow("Частота кадров", fpsBox);

        storageBox = new QComboBox;
        storageBox->addItem("Оперативная память (если доступна)", "ram");
        storageBox->addItem("Диск пользователя", "disk");
        form->addRow("Временный буфер", storageBox);
        encoderBox = new QComboBox;
        populateEncoders();
        form->addRow("Кодирование", encoderBox);

        hotkeyEdit = new QwertyKeySequenceEdit(QKeySequence::fromString("Ctrl+Alt+Z", QKeySequence::PortableText));
        hotkeyEdit->setMaximumSequenceLength(1);
        form->addRow("Горячая клавиша сохранения", hotkeyEdit);
        QString videosPath = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (videosPath.isEmpty()) videosPath = QDir::homePath() + "/Videos";
        outputEdit = new QLineEdit(videosPath + "/Rodip");
        auto *folderButton = new QPushButton("Выбрать…");
        auto *folderRow = new QWidget;
        auto *folderLayout = new QHBoxLayout(folderRow);
        folderLayout->setContentsMargins(0, 0, 0, 0);
        folderLayout->addWidget(outputEdit, 1);
        folderLayout->addWidget(folderButton);
        form->addRow("Папка сохранения", folderRow);
        root->addLayout(form);

        statusLabel = new QLabel;
        statusLabel->setObjectName("status");
        root->addWidget(statusLabel);
        auto *actions = new QHBoxLayout;
        saveButton = new QPushButton("Сохранить повтор");
        saveButton->setEnabled(false);
        actions->addWidget(saveButton);
        root->addLayout(actions);
        auto *hint = new QLabel;
        hint->setWordWrap(true);
        hint->setObjectName("hint");
        hint->setText("Буфер хранит короткие сегменты видео и звука. После нажатия горячей клавиши последние выбранные секунды объединяются в MP4. Аппаратное кодирование зависит от установленного драйвера и FFmpeg.");
        root->addWidget(hint);

        auto *logTitle = new QLabel("События");
        root->addWidget(logTitle);
        logView = new QPlainTextEdit;
        logView->setReadOnly(true);
        logView->setMaximumBlockCount(150);
        root->addWidget(logView, 1);

        connect(folderButton, &QPushButton::clicked, this, &RodipWindow::chooseFolder);
        connect(hotkeyEdit, &QKeySequenceEdit::editingFinished, this, [this] {
            if (hotkeyEdit->keySequence().isEmpty())
                hotkeyEdit->setKeySequence(QKeySequence::fromString("Ctrl+Alt+Z", QKeySequence::PortableText));
            persistSettings();
            setupHotkey();
        });
        connect(saveButton, &QPushButton::clicked, this, &RodipWindow::saveReplay);
        connect(&recorder, &QProcess::readyReadStandardError, this, &RodipWindow::readRecorderLog);
        connect(&recorder, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            if (recording) setError("Не удалось запустить FFmpeg. Установите ffmpeg и проверьте настройки источников.");
        });
        connect(&recorder, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
            if (!recording || code == 0) return;
            recording = false;
            pruneTimer.stop();
            if (waylandInput) waylandCapture.stopCapture();
            saveButton->setEnabled(false);
            setError("Запись остановилась с ошибкой FFmpeg (код " + QString::number(code) + "). Проверьте доступность кодировщика, захвата экрана и аудио.");
        });
        connect(&saveProcess, &QProcess::readyReadStandardError, this, [this] { logView->appendPlainText(QString::fromLocal8Bit(saveProcess.readAllStandardError()).trimmed()); });
        connect(&saveProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &RodipWindow::saveFinished);
        connect(&waylandCapture, &WaylandCapture::formatReady, this, &RodipWindow::startWaylandRecorder);
        connect(&waylandCapture, &WaylandCapture::frameReady, this, [this](const QByteArray &frame) {
            if (!recording || !waylandInput || recorder.state() != QProcess::Running) return;
            const qint64 maxQueued = static_cast<qint64>(displayWidth) * displayHeight * 4 * 3;
            if (recorder.bytesToWrite() < maxQueued) recorder.write(frame);
        });
        connect(&waylandCapture, &WaylandCapture::failed, this, [this](const QString &reason) {
            if (starting || recording) {
                starting = false;
                recording = false;
                pruneTimer.stop();
                waylandCapture.stopCapture();
                if (recorder.state() != QProcess::NotRunning) {
                    recorder.terminate();
                    if (!recorder.waitForFinished(1000)) recorder.kill();
                }
                saveButton->setEnabled(false);
            }
            setError(reason);
        });
        connect(&waylandCapture, &WaylandCapture::saveShortcutActivated, this, &RodipWindow::saveReplay);
        connect(&waylandCapture, &WaylandCapture::shortcutFailed, this, [this](const QString &reason) { log(reason); });
        pruneTimer.setInterval(1000);
        connect(&pruneTimer, &QTimer::timeout, this, &RodipWindow::pruneSegments);
        restoreSettings();
        connect(durationSlider, &QSlider::valueChanged, this, [this](int) {
            updateDurationLabel();
            persistSettings();
        });
        auto restartForCaptureSetting = [this] { persistSettings(); restartWithCurrentSettings(); };
        connect(displayBox, qOverload<int>(&QComboBox::currentIndexChanged), this, restartForCaptureSetting);
        connect(microphoneBox, qOverload<int>(&QComboBox::currentIndexChanged), this, restartForCaptureSetting);
        connect(fpsBox, qOverload<int>(&QComboBox::currentIndexChanged), this, restartForCaptureSetting);
        connect(storageBox, qOverload<int>(&QComboBox::currentIndexChanged), this, restartForCaptureSetting);
        connect(encoderBox, qOverload<int>(&QComboBox::currentIndexChanged), this, restartForCaptureSetting);
        connect(systemAudioCheck, &QCheckBox::toggled, this, restartForCaptureSetting);
        setupHotkey();
        applyStyle();
        updateDurationLabel();
        updateStatus("Запускаю буферизацию…");
        log("Запуск непрерывной буферизации.");
        QTimer::singleShot(0, this, &RodipWindow::startRecording);
    }

    ~RodipWindow() override {
        waylandCapture.stop();
        if (recorder.state() != QProcess::NotRunning) {
            recorder.terminate();
            if (!recorder.waitForFinished(1500)) { recorder.kill(); recorder.waitForFinished(1000); }
        }
#ifdef RODIP_X11
        if (xDisplay) { XUngrabKey(xDisplay, AnyKey, AnyModifier, DefaultRootWindow(xDisplay)); XCloseDisplay(xDisplay); }
#endif
    }

private:
    QComboBox *displayBox{};
    QCheckBox *systemAudioCheck{};
    QComboBox *microphoneBox{};
    QSlider *durationSlider{};
    QLabel *durationValueLabel{};
    QComboBox *fpsBox{};
    QComboBox *storageBox{};
    QComboBox *encoderBox{};
    QwertyKeySequenceEdit *hotkeyEdit{};
    QLineEdit *outputEdit{};
    QPushButton *saveButton{};
    QLabel *statusLabel{};
    QPlainTextEdit *logView{};
    QProcess recorder;
    QProcess saveProcess;
    WaylandCapture waylandCapture;
    QTimer pruneTimer;
    std::unique_ptr<QSettings> settings;
    QString bufferDirectory;
    QString pendingOutput;
    QString vaapiDevice;
    QString defaultMonitorSource;
    bool recording = false;
    bool starting = false;
    bool waylandInput = false;
    bool saving = false;
    int displayX = 0;
    int displayY = 0;
    int displayWidth = 0;
    int displayHeight = 0;
#ifdef RODIP_X11
    Display *xDisplay = nullptr;
    QSocketNotifier *xNotifier = nullptr;
#endif

    void applyStyle() {
        setStyleSheet("QWidget { background:#15181d; color:#e8edf2; font-size:14px; } QLabel#title { font-size:24px; font-weight:700; } QLabel#subtitle,QLabel#hint { color:#a8b2bf; } QLabel#status { color:#91e5b1; font-weight:600; padding:8px 0; } QComboBox,QLineEdit,QPlainTextEdit { background:#20252d; border:1px solid #353d48; border-radius:6px; padding:7px; } QSlider::groove:horizontal { height:6px; background:#353d48; border-radius:3px; } QSlider::handle:horizontal { background:#55bd8b; width:16px; margin:-6px 0; border-radius:8px; } QPushButton { background:#2e7d60; border:0; border-radius:6px; padding:10px 14px; font-weight:600; } QPushButton:hover { background:#36936f; } QPushButton:disabled { background:#343a43; color:#89919b; }");
    }

    void populateAudioSources() {
        QProcess process;
        process.start("pactl", {"list", "sources"});
        QStringList monitorSources;
        QString sourceName;
        QString sourceDescription;
        auto addSource = [&] {
            if (sourceName.isEmpty()) return;
            if (sourceName.endsWith(".monitor")) monitorSources << sourceName;
            else microphoneBox->addItem(sourceDescription.isEmpty() ? sourceName : sourceDescription, sourceName);
        };
        if (process.waitForFinished(2000)) {
            const auto lines = QString::fromLocal8Bit(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const auto &line : lines) {
                const QString trimmed = line.trimmed();
                if (trimmed.startsWith("Source #")) {
                    addSource();
                    sourceName.clear();
                    sourceDescription.clear();
                } else if (trimmed.startsWith("Name:")) {
                    sourceName = trimmed.mid(QStringLiteral("Name:").size()).trimmed();
                } else if (trimmed.startsWith("Description:")) {
                    sourceDescription = trimmed.mid(QStringLiteral("Description:").size()).trimmed();
                }
            }
            addSource();
        }
        if (microphoneBox->count() == 0) {
            QProcess fallback;
            fallback.start("pactl", {"list", "short", "sources"});
            if (fallback.waitForFinished(2000)) {
                const auto lines = QString::fromLocal8Bit(fallback.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
                for (const auto &line : lines) {
                    const auto parts = line.split('\t');
                    if (parts.size() < 2) continue;
                    const QString name = parts[1].trimmed();
                    if (name.endsWith(".monitor")) {
                        if (!monitorSources.contains(name)) monitorSources << name;
                    }
                    else microphoneBox->addItem(name, name);
                }
            }
        }
        microphoneBox->insertItem(0, "Не записывать микрофон", "");
        QProcess defaultSink;
        defaultSink.start("pactl", {"get-default-sink"});
        if (defaultSink.waitForFinished(1500)) {
            const QString sinkName = QString::fromLocal8Bit(defaultSink.readAllStandardOutput()).trimmed();
            const QString name = sinkName.isEmpty() ? QString() : sinkName + ".monitor";
            if (monitorSources.contains(name)) defaultMonitorSource = name;
        }
        if (defaultMonitorSource.isEmpty() && !monitorSources.isEmpty()) defaultMonitorSource = monitorSources.first();
        systemAudioCheck->setEnabled(!defaultMonitorSource.isEmpty());
        systemAudioCheck->setChecked(!defaultMonitorSource.isEmpty());
    }

    QString systemAudioSource() const {
        QProcess defaultSink;
        defaultSink.start("pactl", {"get-default-sink"});
        if (defaultSink.waitForFinished(1500)) {
            const QString sinkName = QString::fromLocal8Bit(defaultSink.readAllStandardOutput()).trimmed();
            if (!sinkName.isEmpty()) return sinkName + ".monitor";
        }
        return defaultMonitorSource;
    }

    void populateEncoders() {
        encoderBox->addItem("Автоматически (аппаратное, если доступно)", "auto");
        encoderBox->addItem("Программно: CPU (x264)", "libx264");
        QProcess process;
        process.start("ffmpeg", {"-hide_banner", "-encoders"});
        if (!process.waitForFinished(2500)) return;
        const QString encoders = QString::fromLocal8Bit(process.readAllStandardOutput()) + QString::fromLocal8Bit(process.readAllStandardError());
        const auto devices = QDir("/dev/dri").entryList({"renderD*"}, QDir::System | QDir::Files, QDir::Name);
        if (encoders.contains("h264_vaapi")) {
            for (const auto &device : devices) {
                const QString path = "/dev/dri/" + device;
                if (!probeEncoder({"-init_hw_device", "vaapi=va:" + path, "-filter_hw_device", "va", "-f", "lavfi", "-i", "color=size=320x240:rate=30", "-vf", "format=nv12,hwupload", "-frames:v", "1", "-c:v", "h264_vaapi", "-f", "null", "-"})) continue;
                vaapiDevice = path;
                encoderBox->addItem("Аппаратно: VAAPI (Intel / AMD)", "h264_vaapi");
                break;
            }
        }
        if (encoders.contains("h264_nvenc") && QFileInfo::exists("/dev/nvidia0")
            && probeEncoder({"-f", "lavfi", "-i", "color=size=320x240:rate=30", "-frames:v", "1", "-c:v", "h264_nvenc", "-f", "null", "-"}))
            encoderBox->addItem("Аппаратно: NVENC (NVIDIA)", "h264_nvenc");
    }

    bool probeEncoder(const QStringList &arguments) const {
        QProcess probe;
        probe.start("ffmpeg", QStringList{"-hide_banner", "-loglevel", "error"} + arguments);
        if (!probe.waitForFinished(6000)) {
            probe.kill();
            probe.waitForFinished(1000);
            return false;
        }
        return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    }

    void restoreSettings() {
        displayBox->setCurrentIndex(std::clamp(settings->value("display", 0).toInt(), 0, displayBox->count() - 1));
        durationSlider->setValue(std::clamp(settings->value("duration", 15).toInt(), 5, 15 * 60));
        fpsBox->setCurrentIndex(std::max(0, fpsBox->findData(settings->value("fps", 30))));
        storageBox->setCurrentIndex(std::max(0, storageBox->findData(settings->value("storage", "ram"))));
        QString savedEncoder = settings->value("encoder", "auto").toString();
        if (savedEncoder == "libx264") savedEncoder = "auto";
        encoderBox->setCurrentIndex(std::max(0, encoderBox->findData(savedEncoder)));
        auto savedHotkey = QKeySequence::fromString(settings->value("hotkey", "Ctrl+Alt+Z").toString(), QKeySequence::PortableText);
        if (savedHotkey.isEmpty()) savedHotkey = QKeySequence::fromString("Ctrl+Alt+Z", QKeySequence::PortableText);
        hotkeyEdit->setKeySequence(savedHotkey);
        outputEdit->setText(settings->value("output", outputEdit->text()).toString());
        const bool recordSystemAudio = settings->value("recordSystemAudio", !defaultMonitorSource.isEmpty()).toBool();
        systemAudioCheck->setChecked(recordSystemAudio && !defaultMonitorSource.isEmpty());
        microphoneBox->setCurrentIndex(std::max(0, microphoneBox->findData(settings->value("microphone", ""))));
        const QString tmpRoot = storageBox->currentData().toString() == "ram" && QDir("/dev/shm").exists()
            ? "/dev/shm" : QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        qint64 requiredRam = static_cast<qint64>(durationSlider->value() + 10) * 4 * 1024 * 1024 + 32 * 1024 * 1024;
        if (tmpRoot == "/dev/shm" && QStorageInfo(tmpRoot).bytesAvailable() > 0 && QStorageInfo(tmpRoot).bytesAvailable() < requiredRam) {
            bufferDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rodip-buffer";
            log("В /dev/shm недостаточно места для выбранной длительности; буфер будет записываться на диск.");
        } else {
            bufferDirectory = tmpRoot + "/rodip-buffer";
        }
    }

    void persistSettings() {
        settings->setValue("display", displayBox->currentIndex());
        settings->setValue("duration", durationSlider->value());
        settings->setValue("fps", fpsBox->currentData());
        settings->setValue("storage", storageBox->currentData());
        settings->setValue("encoder", encoderBox->currentData());
        settings->setValue("hotkey", hotkeyEdit->keySequence().toString(QKeySequence::PortableText));
        settings->setValue("output", outputEdit->text());
        settings->setValue("recordSystemAudio", systemAudioCheck->isChecked());
        settings->setValue("microphone", microphoneBox->currentData());
        settings->sync();
    }

    void chooseFolder() {
        const auto dir = QFileDialog::getExistingDirectory(this, "Папка сохранения", outputEdit->text());
        if (!dir.isEmpty()) outputEdit->setText(dir);
    }

    void startRecording() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
            QMessageBox::warning(this, "FFmpeg не найден", "Установите FFmpeg из репозитория вашего дистрибутива.");
            return;
        }
        const bool isWayland = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
            || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland"
            || QGuiApplication::platformName().contains("wayland", Qt::CaseInsensitive);
        persistSettings();
        const bool useRam = storageBox->currentData().toString() == "ram" && QDir("/dev/shm").exists();
        const QString tmpRoot = useRam ? "/dev/shm" : QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const qint64 requiredSpace = static_cast<qint64>(durationSlider->value() + 10) * 4 * 1024 * 1024 + 32 * 1024 * 1024;
        if (useRam && QStorageInfo(tmpRoot).bytesAvailable() < requiredSpace) {
            bufferDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rodip-buffer";
            log("В /dev/shm недостаточно места для выбранной длительности; буфер будет записываться на диск.");
        } else {
            bufferDirectory = tmpRoot + "/rodip-buffer";
        }
        if (isWayland) {
            starting = true;
            waylandInput = true;
            updateStatus("Ожидаю разрешение портала на захват экрана…");
            log("Запрашиваю доступ к экрану через XDG Desktop Portal.");
            waylandCapture.start(fpsBox->currentData().toInt());
            return;
        }
        auto *screen = QGuiApplication::screens().value(displayBox->currentIndex(), nullptr);
        if (!screen) { setError("Выбранный экран больше недоступен."); return; }
        const auto geometry = screen->geometry();
        displayX = geometry.x(); displayY = geometry.y(); displayWidth = geometry.width(); displayHeight = geometry.height();
        waylandInput = false;
        startEncoderAndSegments(false, "");
    }

    void startWaylandRecorder(int width, int height, const QString &pixelFormat) {
        if (!starting) return;
        displayWidth = width; displayHeight = height;
        startEncoderAndSegments(true, pixelFormat);
    }

    QString selectedEncoder() const {
        const QString selected = encoderBox->currentData().toString();
        if (selected != "auto") return selected;
        if (encoderBox->findData("h264_nvenc") >= 0) return "h264_nvenc";
        if (encoderBox->findData("h264_vaapi") >= 0) return "h264_vaapi";
        return "libx264";
    }

    void startEncoderAndSegments(bool portalCapture, const QString &pixelFormat) {
        QDir().mkpath(bufferDirectory);
        QDir dir(bufferDirectory);
        for (const auto &f : dir.entryList({"*.ts", "concat.txt"}, QDir::Files)) dir.remove(f);

        const QString encoder = selectedEncoder();
        QStringList args{"-hide_banner", "-loglevel", "warning", "-y"};
        if (encoder == "h264_vaapi") args << "-init_hw_device" << "vaapi=va:" + vaapiDevice << "-filter_hw_device" << "va";
        args << "-thread_queue_size" << "512";
        if (portalCapture) {
            args << "-f" << "rawvideo" << "-pixel_format" << pixelFormat << "-video_size"
                 << QString("%1x%2").arg(displayWidth).arg(displayHeight) << "-framerate"
                 << QString::number(fpsBox->currentData().toInt()) << "-i" << "pipe:0";
        } else {
            args << "-f" << "x11grab" << "-framerate" << QString::number(fpsBox->currentData().toInt())
                 << "-video_size" << QString("%1x%2").arg(displayWidth).arg(displayHeight)
                 << "-i" << QString("%1+%2,%3").arg(qEnvironmentVariable("DISPLAY", ":0")).arg(displayX).arg(displayY);
        }
        QStringList audioLabels;
        const QString sysSource = systemAudioCheck->isChecked() ? systemAudioSource() : QString();
        const QString micSource = microphoneBox->currentData().toString();
        if (!sysSource.isEmpty()) { args << "-thread_queue_size" << "512" << "-f" << "pulse" << "-i" << sysSource; audioLabels << "[1:a]"; }
        if (!micSource.isEmpty()) { args << "-thread_queue_size" << "512" << "-f" << "pulse" << "-i" << micSource; audioLabels << QString("[%1:a]").arg(audioLabels.isEmpty() ? 1 : 2); }
        args << "-map" << "0:v:0";
        if (audioLabels.size() == 1) args << "-map" << audioLabels.first().mid(1, audioLabels.first().size() - 2);
        if (audioLabels.size() > 1) args << "-filter_complex" << audioLabels.join("") + "amix=inputs=2:duration=longest:dropout_transition=2[aout]" << "-map" << "[aout]";
        if (encoder == "libx264") args << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-tune" << "zerolatency" << "-crf" << "26";
        else if (encoder == "h264_vaapi") args << "-vf" << "format=nv12,hwupload" << "-c:v" << encoder << "-qp" << "24";
        else args << "-c:v" << encoder << "-preset" << "p1" << "-cq" << "24";
        args << "-r" << QString::number(fpsBox->currentData().toInt()) << "-g" << QString::number(fpsBox->currentData().toInt())
             << "-force_key_frames" << "expr:gte(t,n_forced*1)";
        if (!audioLabels.isEmpty()) args << "-c:a" << "aac" << "-b:a" << "160k";
        else args << "-an";
        args << "-f" << "segment" << "-segment_time" << "1" << "-segment_format" << "mpegts" << "-reset_timestamps" << "1"
             << bufferDirectory + "/%08d.ts";
        recorder.setProgram("ffmpeg");
        recorder.setArguments(args);
        log("Запуск FFmpeg: " + args.join(' '));
        recorder.start(QIODevice::ReadWrite);
        if (!recorder.waitForStarted(1500)) {
            starting = false;
            if (portalCapture) waylandCapture.stopCapture();
            setError("FFmpeg не удалось запустить.");
            return;
        }
        starting = false;
        recording = true;
        pruneTimer.start();
        saveButton->setEnabled(true);
        updateStatus("Идёт запись буфера");
        log("Кодировщик: " + encoder + ".");
        log(QString("Буферизация запущена: %1×%2, %3 FPS.").arg(displayWidth).arg(displayHeight).arg(fpsBox->currentData().toInt()));
    }

    void restartWithCurrentSettings() {
        if (!recording && !starting) return;
        starting = false;
        recording = false;
        pruneTimer.stop();
        if (waylandInput) waylandCapture.stopCapture();
        if (recorder.state() != QProcess::NotRunning) {
            recorder.terminate();
            if (!recorder.waitForFinished(1200)) { recorder.kill(); recorder.waitForFinished(1000); }
        }
        saveButton->setEnabled(false);
        updateStatus("Перезапускаю буфер с новыми настройками…");
        QTimer::singleShot(0, this, &RodipWindow::startRecording);
    }

    QStringList recentSegments() const {
        QDir dir(bufferDirectory);
        auto files = dir.entryInfoList({"*.ts"}, QDir::Files, QDir::Name);
        QStringList paths;
        for (const auto &file : files) paths << file.absoluteFilePath();
        return paths;
    }

    void saveReplay() {
        if (saving) return;
        auto files = recentSegments();
        if (recording && !files.isEmpty()) files.removeLast();
        const int seconds = durationSlider->value();
        if (files.size() < std::min(2, seconds)) { setError("Буфер ещё не набрал достаточно видеосегментов. Подождите несколько секунд."); return; }
        QDir().mkpath(outputEdit->text());
        const int count = std::min(seconds, static_cast<int>(files.size()));
        const QString listPath = bufferDirectory + "/concat.txt";
        QFile list(listPath);
        if (!list.open(QIODevice::WriteOnly | QIODevice::Text)) { setError("Не удалось создать временный список сегментов."); return; }
        for (int i = files.size() - count; i < files.size(); ++i) {
            QString escaped = files[i]; escaped.replace("'", "'\\''");
            list.write(QString("file '%1'\n").arg(escaped).toUtf8());
        }
        list.close();
        pendingOutput = outputEdit->text() + "/Rodip_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz") + ".mp4";
        saveProcess.setProgram("ffmpeg");
        saveProcess.setArguments({"-hide_banner", "-loglevel", "warning", "-y", "-f", "concat", "-safe", "0", "-i", listPath, "-c", "copy", "-movflags", "+faststart", pendingOutput});
        saveProcess.start();
        if (!saveProcess.waitForStarted(1500)) { setError("Не удалось запустить FFmpeg для сохранения."); return; }
        saving = true;
        saveButton->setEnabled(false);
        updateStatus("Сохраняю последний повтор…");
        log("Сохраняю последние " + QString::number(count) + " сегментов.");
    }

    void saveFinished(int code, QProcess::ExitStatus status) {
        saving = false;
        saveButton->setEnabled(recording);
        if (code == 0 && status == QProcess::NormalExit && QFileInfo::exists(pendingOutput)) {
            updateStatus("Повтор сохранён");
            log("Файл сохранён: " + pendingOutput);
        } else {
            updateStatus(recording ? "Идёт запись буфера" : "Буферизация остановлена");
            log("Ошибка при сохранении повтора. Проверьте аудиокодек и FFmpeg.");
        }
    }

    void pruneSegments() {
        const auto files = recentSegments();
        const int keep = durationSlider->value() + 10;
        for (int i = 0; i < files.size() - keep; ++i) QFile::remove(files[i]);
    }

    void updateDurationLabel() {
        const int seconds = durationSlider->value();
        if (seconds < 60) {
            const int lastDigit = seconds % 10;
            const int lastTwoDigits = seconds % 100;
            const QString unit = lastDigit == 1 && lastTwoDigits != 11 ? "секунда"
                : lastDigit >= 2 && lastDigit <= 4 && (lastTwoDigits < 12 || lastTwoDigits > 14) ? "секунды" : "секунд";
            durationValueLabel->setText(QString("%1 %2").arg(seconds).arg(unit));
            return;
        }
        const int minutes = seconds / 60;
        const int remainder = seconds % 60;
        const int lastDigit = minutes % 10;
        const QString unit = lastDigit == 1 && minutes != 11 ? "минута"
            : lastDigit >= 2 && lastDigit <= 4 && minutes != 12 && minutes != 13 && minutes != 14 ? "минуты" : "минут";
        durationValueLabel->setText(remainder ? QString("%1 %2 %3 с").arg(minutes).arg(unit).arg(remainder)
                                               : QString("%1 %2").arg(minutes).arg(unit));
    }

    void readRecorderLog() {
        const auto text = QString::fromLocal8Bit(recorder.readAllStandardError()).trimmed();
        if (!text.isEmpty()) log(text);
    }

    void setupHotkey() {
        const bool waylandSession = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
            || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland"
            || QGuiApplication::platformName().contains("wayland", Qt::CaseInsensitive);
        if (waylandSession) {
            waylandCapture.bindSaveShortcut(hotkeyEdit->keySequence().toString(QKeySequence::PortableText).toUpper());
            return;
        }
#ifdef RODIP_X11
        if (QGuiApplication::platformName() != "xcb") {
            log("Глобальные горячие клавиши на Wayland доступны через системный портал после подключения Wayland backend.");
            return;
        }
        if (!xDisplay) xDisplay = XOpenDisplay(nullptr);
        if (!xDisplay) { log("Не удалось подключить X11 для глобальной горячей клавиши."); return; }
        const auto combination = hotkeyEdit->keySequence()[0];
        const int key = combination.key();
        unsigned int modifiers = 0;
        const auto qtModifiers = combination.keyboardModifiers();
        if (qtModifiers & Qt::ControlModifier) modifiers |= ControlMask;
        if (qtModifiers & Qt::ShiftModifier) modifiers |= ShiftMask;
        if (qtModifiers & Qt::AltModifier) modifiers |= Mod1Mask;
        if (qtModifiers & Qt::MetaModifier) modifiers |= Mod4Mask;
        const KeySym symbol = keySymFromQtKey(key);
        const KeyCode code = XKeysymToKeycode(xDisplay, symbol);
        if (!code) { setError("X11 не распознал выбранную клавишу."); return; }
        XUngrabKey(xDisplay, AnyKey, AnyModifier, DefaultRootWindow(xDisplay));
        const QList<unsigned int> lockMasks{0u, static_cast<unsigned int>(LockMask), static_cast<unsigned int>(Mod2Mask), static_cast<unsigned int>(LockMask | Mod2Mask)};
        for (const unsigned int lockMask : lockMasks)
            XGrabKey(xDisplay, code, modifiers | lockMask, DefaultRootWindow(xDisplay), True, GrabModeAsync, GrabModeAsync);
        XSelectInput(xDisplay, DefaultRootWindow(xDisplay), KeyPressMask);
        XFlush(xDisplay);
        if (!xNotifier) {
            xNotifier = new QSocketNotifier(ConnectionNumber(xDisplay), QSocketNotifier::Read, this);
            connect(xNotifier, &QSocketNotifier::activated, this, [this] {
                while (XPending(xDisplay)) {
                    XEvent event; XNextEvent(xDisplay, &event);
                    if (event.type == KeyPress) saveReplay();
                }
            });
        }
#else
        log("Сборка без X11: глобальная горячая клавиша недоступна.");
#endif
    }

#ifdef RODIP_X11
    KeySym keySymFromQtKey(int key) const {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            const char letter = static_cast<char>('a' + key - Qt::Key_A);
            return XStringToKeysym(QByteArray(1, letter).constData());
        }
        if (key >= Qt::Key_0 && key <= Qt::Key_9) return XK_0 + key - Qt::Key_0;
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35) return XK_F1 + key - Qt::Key_F1;
        switch (key) {
        case Qt::Key_Space: return XK_space;
        case Qt::Key_Escape: return XK_Escape;
        case Qt::Key_Tab: return XK_Tab;
        case Qt::Key_Backtab: return XK_ISO_Left_Tab;
        case Qt::Key_Return: return XK_Return;
        case Qt::Key_Enter: return XK_KP_Enter;
        case Qt::Key_Backspace: return XK_BackSpace;
        case Qt::Key_Delete: return XK_Delete;
        case Qt::Key_Insert: return XK_Insert;
        case Qt::Key_Home: return XK_Home;
        case Qt::Key_End: return XK_End;
        case Qt::Key_Left: return XK_Left;
        case Qt::Key_Right: return XK_Right;
        case Qt::Key_Up: return XK_Up;
        case Qt::Key_Down: return XK_Down;
        case Qt::Key_PageUp: return XK_Page_Up;
        case Qt::Key_PageDown: return XK_Page_Down;
        case Qt::Key_Print: return XK_Print;
        case Qt::Key_Pause: return XK_Pause;
        case Qt::Key_Menu: return XK_Menu;
        default: return NoSymbol;
        }
    }
#endif

    void updateStatus(const QString &message) { statusLabel->setText("● " + message); }
    void setError(const QString &message) { updateStatus(message); log(message); }
    void log(const QString &message) { logView->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + message); }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Rodip");
    QApplication::setOrganizationName("Rodip");
    RodipWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"
