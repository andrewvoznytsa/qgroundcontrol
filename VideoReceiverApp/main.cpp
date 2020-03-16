#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <QQuickWindow>
#include <QQuickItem>
#include <QRunnable>
#include <QCommandLineParser>

#if defined(__android__)
#include <QtAndroidExtras>

#include <jni.h>

#include <android/log.h>

static jobject _class_loader = nullptr;
static jobject _context = nullptr;

extern "C" {
    void gst_amc_jni_set_java_vm(JavaVM *java_vm);

    jobject gst_android_get_application_class_loader(void) {
        return _class_loader;
    }
}

static void
gst_android_init(JNIEnv* env, jobject context)
{
    jobject class_loader = nullptr;

    jclass context_cls = env->GetObjectClass(context);

    if (!context_cls) {
        return;
    }

    jmethodID get_class_loader_id = env->GetMethodID(context_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    class_loader = env->CallObjectMethod(context, get_class_loader_id);

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return;
    }

    _context = env->NewGlobalRef(context);
    _class_loader = env->NewGlobalRef(class_loader);
}

static const char kJniClassName[] {"labs/mavlink/VideoReceiverApp/QGLSinkActivity"};

static void setNativeMethods(void)
{
    JNINativeMethod javaMethods[] {
        {"nativeInit", "()V", reinterpret_cast<void *>(gst_android_init)}
    };

    QAndroidJniEnvironment jniEnv;

    if (jniEnv->ExceptionCheck()) {
        jniEnv->ExceptionDescribe();
        jniEnv->ExceptionClear();
    }

    jclass objectClass = jniEnv->FindClass(kJniClassName);

    if (!objectClass) {
        qWarning() << "Couldn't find class:" << kJniClassName;
        return;
    }

    jint val = jniEnv->RegisterNatives(objectClass, javaMethods, sizeof(javaMethods) / sizeof(javaMethods[0]));

    if (val < 0) {
        qWarning() << "Error registering methods: " << val;
    } else {
        qDebug() << "Main Native Functions Registered";
    }

    if (jniEnv->ExceptionCheck()) {
        jniEnv->ExceptionDescribe();
        jniEnv->ExceptionClear();
    }
}

jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    Q_UNUSED(reserved);

    JNIEnv* env;

    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }

    setNativeMethods();

    gst_amc_jni_set_java_vm(vm);

    return JNI_VERSION_1_6;
}
#endif

#include <VideoReceiver.h>

class StartDecoding : public QRunnable
{
public:
    StartDecoding(VideoReceiver* receiver, QQuickItem* widget)
        : _receiver(receiver)
        , _widget(widget)
    {}

    void run();

private:
    VideoReceiver* _receiver;
    QQuickItem* _widget;
};

void
StartDecoding::run()
{
    _receiver->startDecoding(createVideoSink(_widget));
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QCommandLineParser parser;

    parser.addHelpOption();

    parser.addPositionalArgument("url",
        QGuiApplication::translate("main", "Source URL."));

    QCommandLineOption timeoutOption(QStringList() << "t" << "timeout",
        QGuiApplication::translate("main", "Source timeout."),
        QGuiApplication::translate("main", "seconds"));

    parser.addOption(timeoutOption);

    QCommandLineOption noDecodeOption(QStringList() << "n" << "no-decode",
        QGuiApplication::translate("main", "Don't decode and render video."));

    parser.addOption(noDecodeOption);

    QCommandLineOption stopDecodingOption("stop-decoding",
        QGuiApplication::translate("main", "Stop decoding after time."),
        QGuiApplication::translate("main", "seconds"));

    parser.addOption(stopDecodingOption);

    QCommandLineOption recordOption(QStringList() << "r" << "record",
        QGuiApplication::translate("main", "Record video."),
        QGuiApplication::translate("main", "file"));

    parser.addOption(recordOption);

    QCommandLineOption formatOption(QStringList() << "f" << "format",
        QGuiApplication::translate("main", "File format."),
        QGuiApplication::translate("main", "format"));

    parser.addOption(formatOption);

    QCommandLineOption stopRecordingOption("stop-recording",
        QGuiApplication::translate("main", "Stop recording after time."),
        QGuiApplication::translate("main", "seconds"));

    parser.addOption(stopRecordingOption);

    parser.process(app);

    const QStringList args = parser.positionalArguments();

    if (args.size() != 1) {
        parser.showHelp(0);
    }

    QString url = args.at(0);
    unsigned timeout = 5;
    bool decode = true;
    unsigned stopDecodingAfter = 0;
    bool record = false;
    QString videoFile;
    unsigned int fileFormat = VideoReceiver::FILE_FORMAT_MIN;
    unsigned stopRecordingAfter = 15;

    if (parser.isSet(timeoutOption)) {
        timeout = parser.value(timeoutOption).toUInt();
    }

    if (parser.isSet(noDecodeOption)) {
        decode = false;
    }

    if (decode && parser.isSet(stopDecodingOption)) {
        stopDecodingAfter = parser.value(stopDecodingOption).toUInt();
    }

    if (parser.isSet(recordOption)) {
        record = true;
        videoFile = parser.value(recordOption);
    }

    if (parser.isSet(formatOption)) {
        fileFormat += parser.value(formatOption).toUInt();
    }

    if (record && parser.isSet(stopRecordingOption)) {
        stopRecordingAfter = parser.value(stopRecordingOption).toUInt();
    }

    initializeVideoReceiver(argc, argv, 3);

    VideoReceiver* receiver = new VideoReceiver();

    receiver->start(url, timeout);

    QQmlApplicationEngine engine;

    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));

    QQuickWindow* rootObject = static_cast<QQuickWindow*>(engine.rootObjects().first());
    Q_ASSERT(rootObject != nullptr);

    QQuickItem* videoItem = rootObject->findChild<QQuickItem*>("videoItem");
    Q_ASSERT(videoItem != nullptr);

    if (decode) {
        rootObject->scheduleRenderJob(new StartDecoding(receiver, videoItem), QQuickWindow::BeforeSynchronizingStage);

        if (stopDecodingAfter > 0) {
            QTimer::singleShot(stopDecodingAfter * 1000, [receiver](){
                receiver->stopDecoding();
            });
        }
    }

    if (record) {
        receiver->startRecording(videoFile, static_cast<VideoReceiver::FILE_FORMAT>(fileFormat));

        if (stopRecordingAfter > 0) {
            QTimer::singleShot(stopRecordingAfter * 1000, [receiver](){
                receiver->stopRecording();
            });
        }
    }

    return app.exec();
}
