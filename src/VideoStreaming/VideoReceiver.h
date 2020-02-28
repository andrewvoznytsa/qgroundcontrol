/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

/**
 * @file
 *   @brief QGC Video Receiver
 *   @author Gus Grubba <gus@auterion.com>
 */

#pragma once

#include "QGCLoggingCategory.h"
#include <QObject>
#include <QSize>
#include <QTimer>
#include <QTcpSocket>

#if defined(QGC_GST_STREAMING)
#include <gst/gst.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(VideoReceiverLog)

class VideoSettings;

class VideoReceiver : public QObject
{
    Q_OBJECT
public:

#if defined(QGC_GST_STREAMING)
    Q_PROPERTY(bool             recording           READ    recording           NOTIFY  recordingChanged)
#endif
    Q_PROPERTY(bool             videoRunning        READ    videoRunning        NOTIFY  videoRunningChanged)
    Q_PROPERTY(QString          imageFile           READ    imageFile           NOTIFY  imageFileChanged)
    Q_PROPERTY(QString          videoFile           READ    videoFile           NOTIFY  videoFileChanged)
    Q_PROPERTY(QString          imagePath           READ    imagePath           NOTIFY  imagePathChanged)
    Q_PROPERTY(QString          videoPath           READ    videoPath           NOTIFY  videoPathChanged)

    Q_PROPERTY(bool             showFullScreen      READ    showFullScreen      WRITE   setShowFullScreen     NOTIFY showFullScreenChanged)
    Q_PROPERTY(bool             streamEnabled       READ    streamEnabled       WRITE   setStreamEnabled      NOTIFY streamEnabledChanged)
    Q_PROPERTY(bool             streamConfigured    READ    streamConfigured    WRITE   setStreamConfigured   NOTIFY streamConfiguredChanged)
    Q_PROPERTY(bool             isTaisync           READ    isTaisync           WRITE   setIsTaysinc          NOTIFY isTaisyncChanged)

    Q_PROPERTY(int              recordingFormatId   READ    recordingFormatId   WRITE   setRecordingFormatId  NOTIFY recordingFormatIdChanged)
    Q_PROPERTY(int              rtspTimeout         READ    rtspTimeout         WRITE   setRtspTimeout        NOTIFY rtspTimeoutChanged)

    Q_PROPERTY(QSize            videoSize           READ    videoSize           NOTIFY  videoSizeChanged)

    explicit VideoReceiver(QObject* parent = nullptr);
    ~VideoReceiver();

    bool streamEnabled() const;
    Q_SLOT void setStreamEnabled(bool enabled);
    Q_SIGNAL void streamEnabledChanged();

    bool streamConfigured() const;
    Q_SLOT void setStreamConfigured(bool enabled);
    Q_SIGNAL void streamConfiguredChanged();

    bool isTaisync() const;
    Q_SLOT void setIsTaysinc(bool value);
    Q_SIGNAL void isTaisyncChanged();

    QString videoPath() const;
    Q_SLOT void setVideoPath(const QString& path);
    Q_SIGNAL void videoPathChanged();

    QString imagePath() const;
    Q_SLOT void setImagePath(const QString& path);
    Q_SIGNAL void imagePathChanged();

    int recordingFormatId() const;
    Q_SLOT void setRecordingFormatId(int value);
    Q_SIGNAL void recordingFormatIdChanged();

    int rtspTimeout() const;
    Q_SLOT void setRtspTimeout(int value);
    Q_SIGNAL void rtspTimeoutChanged();

    Q_SIGNAL void restartTimeout();
    Q_SIGNAL void sendMessage(const QString& message);

    // Emitted before recording starts.
    Q_SIGNAL void beforeRecording();
    void setUnittestMode(bool runUnitTests);
#if defined(QGC_GST_STREAMING)
    virtual bool            recording       () { return _recording; }
#endif

    virtual bool            videoRunning    () { return _videoRunning; }
    virtual QString         imageFile       () { return _imageFile; }
    virtual QString         videoFile       () { return _videoFile; }
    virtual bool            showFullScreen  () { return _showFullScreen; }

    virtual void            grabImage       (QString imageFile);

    virtual void        setShowFullScreen   (bool show) { _showFullScreen = show; emit showFullScreenChanged(); }

    virtual QSize           videoSize       () { return _videoSize; }

#if defined(QGC_GST_STREAMING)
    void                  setVideoSink      (GstElement* videoSink);
#endif

    typedef enum {
        FILE_FORMAT_MIN = 0,
        FILE_FORMAT_MKV = FILE_FORMAT_MIN,
        FILE_FORMAT_MOV,
        FILE_FORMAT_MP4,
        FILE_FORMAT_MAX
    } FILE_FORMAT;

signals:
    void videoRunningChanged                ();
    void imageFileChanged                   ();
    void videoFileChanged                   ();
    void showFullScreenChanged              ();
    void videoSizeChanged                   ();
#if defined(QGC_GST_STREAMING)
    void recordingChanged                   ();
    void msgErrorReceived                   ();
    void msgEOSReceived                     ();
    void msgStateChangedReceived            ();
    void gotFirstRecordingKeyFrame          ();
#endif

public slots:
    virtual void start                      ();
    virtual void stop                       ();
    virtual void setUri                     (const QString& uri);
    virtual void stopRecording              ();
    virtual void startRecording             (const QString& videoFile = QString());

    virtual void start_(const QString& uri);
    virtual void stop_(void);
    virtual void startRecording_(const QString& videoFile, FILE_FORMAT format);
    virtual void stopRecording_(void);
    virtual void startDecoding_(GstElement* videoSink);
    virtual void stopDecoding_(void);

protected slots:
    virtual void _updateTimer               ();
#if defined(QGC_GST_STREAMING)
    virtual void _handleError               ();
    virtual void _handleEOS                 ();
    virtual void _handleStateChanged        ();
#endif

protected:

#if defined(QGC_GST_STREAMING)
    virtual GstElement* _makeSource(const QString& uri);
    virtual GstElement* _makeDecoder(GstCaps* caps, GstElement* videoSink);

    virtual GstElement* _makeFileSink(const QString& videoFile, FILE_FORMAT format);

    static void _onNewPad(GstElement* element, GstPad* pad, gpointer data);
    void _onNewSourcePad(GstPad* pad);
    void _onNewDecoderPad(GstPad* pad);
    bool _addDecoder(GstPad* pad);
    bool _addVideoSink(GstPad* pad);
    void _scheduleUnlink(GstElement* from);

    bool                _decoding;
    bool                _removingDecoder;
    bool                _removingRecorder;

    bool                _running;
    bool                _recording;
    bool                _streaming;
    bool                _starting;
    bool                _stopping;
    bool                _stop;
    GstElement*         _source;
    GstElement*         _tee;
    GstElement*         _decoderQueue;
    GstElement*         _recorderQueue;
    GstElement*         _decoder;
    GstElement*         _videoSink;
    GstElement*         _fileSink;

    void _noteVideoSinkFrame                            ();

    static gboolean             _onBusMessage           (GstBus* bus, GstMessage* message, gpointer user_data);
    static GstPadProbeReturn    _unlinkBranch           (GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static GstPadProbeReturn    _videoSinkProbe         (GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static GstPadProbeReturn    _keyframeWatch          (GstPad* pad, GstPadProbeInfo* info, gpointer user_data);

    virtual void                _unlinkBranch           (GstPad* src);
    virtual void                _shutdownDecodingBranch ();
    virtual void                _shutdownRecordingBranch();
    virtual void                _shutdownPipeline       ();

    void                        _setVideoSize           (const QSize& size) { _videoSize = size; emit videoSizeChanged(); }

    GstElement*     _pipeline;
    guint64         _lastFrameId;
    qint64          _lastFrameTime;

    //-- Wait for Video Server to show up before starting
    QTimer          _frameTimer;
    QTimer          _restart_timer;
    int             _restart_time_ms;

    //-- RTSP UDP reconnect timeout
    uint64_t        _udpReconnect_us;
#endif

    QString         _uri;
    QString         _imageFile;
    QString         _videoFile;
    QString         _videoPath;
    QString         _imagePath;

    bool            _videoRunning;
    bool            _showFullScreen;
    bool            _streamEnabled;
    bool            _streamConfigured;
    bool            _storageLimit;
    bool            _unittTestMode;
    bool            _isTaisync;
    int             _recordingFormatId; // 0 - 2, defined in VideoReceiver.cc / kVideoExtensions. TODO: use a better representation.
    int             _rtspTimeout;
    QSize           _videoSize;
};

