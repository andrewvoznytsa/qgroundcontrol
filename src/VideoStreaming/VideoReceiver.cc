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

#include "VideoReceiver.h"

#include <QDebug>
#include <QUrl>
#include <QDateTime>
#include <QSysInfo>

QGC_LOGGING_CATEGORY(VideoReceiverLog, "VideoReceiverLog")

//-----------------------------------------------------------------------------
// Our pipeline look like this:
//
//              +-->_decoderQueue-->[_decoder-->_videoSink]
//              |
// _source-->_tee
//              |
//              +-->_recorderQueue-->[_fileSink]
//

VideoReceiver::VideoReceiver(QObject* parent)
    : QObject(parent)
#if defined(QGC_GST_STREAMING)
    , _running(false)
    , _starting(false)
    , _stopping(false)
    , _removingDecoder(false)
    , _removingRecorder(false)
    , _stop(true)
    , _source(nullptr)
    , _tee(nullptr)
    , _decoderQueue(nullptr)
    , _decoder(nullptr)
    , _videoSink(nullptr)
    , _fileSink(nullptr)
    , _pipeline(nullptr)
    , _lastFrameId(G_MAXUINT64)
    , _lastFrameTime(0)
    , _restart_time_ms(1389)
    , _udpReconnect_us(5000000)
#endif
    , _streaming(false)
    , _decoding(false)
    , _recording(false)
{
#if defined(QGC_GST_STREAMING)
    _restart_timer.setSingleShot(true);
    connect(&_restart_timer, &QTimer::timeout, this, &VideoReceiver::restartTimeout);
    connect(&_frameTimer, &QTimer::timeout, this, &VideoReceiver::_updateTimer);
    _frameTimer.start(1000);
#endif
}

VideoReceiver::~VideoReceiver(void)
{
    stop();
}

void
VideoReceiver::start(const QString& uri, unsigned timeout)
{
    if (uri.isEmpty()) {
        qCDebug(VideoReceiverLog) << "Failed because URI is not specified";
        return;
    }

#if defined(QGC_GST_STREAMING)
    _stop = false;

    if(_running) {
        qCDebug(VideoReceiverLog) << "Already running!";
        return;
    }

    _timeout = timeout;

    _starting = true;

    bool running    = false;
    bool pipelineUp = false;

    do {
        if((_tee = gst_element_factory_make("tee", nullptr)) == nullptr)  {
            qCCritical(VideoReceiverLog) << "gst_element_factory_make('tee') failed";
            break;
        }

        if((_decoderQueue = gst_element_factory_make("queue", nullptr)) == nullptr)  {
            qCCritical(VideoReceiverLog) << "gst_element_factory_make('queue') failed";
            break;
        }

        if((_recorderQueue = gst_element_factory_make("queue", nullptr)) == nullptr)  {
            qCCritical(VideoReceiverLog) << "gst_element_factory_make('queue') failed";
            break;
        }

        if ((_pipeline = gst_pipeline_new("receiver")) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_pipeline_new() failed";
            break;
        }

        g_object_set(_pipeline, "message-forward", TRUE, nullptr);

        if ((_source = _makeSource(uri)) == nullptr) {
            qCCritical(VideoReceiverLog) << "_makeSource() failed";
            break;
        }

        g_signal_connect(_source, "pad-added", G_CALLBACK(_onNewPad), this);

        gst_bin_add_many(GST_BIN(_pipeline), _source, _tee, _decoderQueue, _recorderQueue, nullptr);

        pipelineUp = true;

        if(!gst_element_link(_tee, _decoderQueue)) {
            qCCritical(VideoReceiverLog) << "Unable to link decoder queue";
            break;
        }

        if(!gst_element_link(_tee, _recorderQueue)) {
            qCCritical(VideoReceiverLog) << "Unable to link recorder queue";
            break;
        }

        GstBus* bus = nullptr;

        if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != nullptr) {
            gst_bus_enable_sync_message_emission(bus);
            g_signal_connect(bus, "sync-message", G_CALLBACK(_onBusMessage), this);
            gst_object_unref(bus);
            bus = nullptr;
        }

        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-paused");
        running = gst_element_set_state(_pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    } while(0);

    if (!running) {
        qCCritical(VideoReceiverLog) << "Failed";

        // In newer versions, the pipeline will clean up all references that are added to it
        if (_pipeline != nullptr) {
            gst_bin_remove(GST_BIN(_pipeline), _videoSink);
            gst_object_unref(_pipeline);
            _pipeline = nullptr;
        }

        // If we failed before adding items to the pipeline, then clean up
        if (!pipelineUp) {
            if (_recorderQueue != nullptr) {
                gst_object_unref(_recorderQueue);
                _recorderQueue = nullptr;
            }

            if (_decoderQueue != nullptr) {
                gst_object_unref(_decoderQueue);
                _decoderQueue = nullptr;
            }

            if (_tee != nullptr) {
                gst_object_unref(_tee);
                _tee = nullptr;
            }

            if (_source != nullptr) {
                gst_object_unref(_source);
                _source = nullptr;
            }
        }

        _running = false;
    } else {
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-playing");
        _running = true;
        qCDebug(VideoReceiverLog) << "Running";
    }
    _starting = false;
#endif
}

void
VideoReceiver::stop(void)
{
#if defined(QGC_GST_STREAMING)
    _stop = true;
    qCDebug(VideoReceiverLog) << "Stopping";
    if(!_streaming) {
        _shutdownPipeline();
    } else if (_pipeline != nullptr && !_stopping) {
        qCDebug(VideoReceiverLog) << "Stopping _pipeline";
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline));
        gst_bus_disable_sync_message_emission(bus);
        gst_element_send_event(_pipeline, gst_event_new_eos());
        _stopping = true;
        GstMessage* message = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
        gst_object_unref(bus);
        if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            _shutdownPipeline();
            qCCritical(VideoReceiverLog) << "Error stopping pipeline!";
        } else if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            _handleEOS();
        }
        gst_message_unref(message);
    }
#endif
}

void
VideoReceiver::startDecoding(VideoSink* videoSink)
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "Starting decoding";

    if (_pipeline == nullptr) {
        if (_videoSink != nullptr) {
            gst_object_unref(_videoSink);
            _videoSink = nullptr;
        }
    }

    if(_videoSink != nullptr || _decoding) {
        qCDebug(VideoReceiverLog) << "Already decoding!";
        return;
    }

    GstPad* pad;

    if ((pad = gst_element_get_static_pad(videoSink, "sink")) == nullptr) {
        qCCritical(VideoReceiverLog) << "Unable to find sink pad of video sink";
        return;
    }

    _lastFrameId = G_MAXUINT64;
    _lastFrameTime = 0;

    gst_pad_add_probe(pad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BUFFER), _videoSinkProbe, this, nullptr);
    gst_object_unref(pad);
    pad = nullptr;

    _videoSink = videoSink;
    gst_object_ref(_videoSink);

    _removingDecoder = false;

    if (!_streaming) {
        return;
    }

    GstPad* srcpad;

    if ((srcpad = gst_element_get_static_pad(_decoderQueue, "src")) == nullptr) {
        qCCritical(VideoReceiverLog) << "gst_element_get_static_pad() failed";
        return;
    }

    _decoding = _addDecoder(srcpad);

    gst_object_unref(srcpad);
    srcpad = nullptr;

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-decoding-start");

    if (!_decoding) {
        qCCritical(VideoReceiverLog) << "_addDecoder() failed";
        return;
    }

    qCDebug(VideoReceiverLog) << "Decoding started";
#else
    Q_UNUSED(videoSink)
#endif
}

void
VideoReceiver::stopDecoding(void)
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "Stopping decoding";

    // exit immediately if we are not decoding
    if (_pipeline == nullptr || !_decoding) {
        qCDebug(VideoReceiverLog) << "Not decoding!";
        return;
    }

    _removingDecoder = true;

    _scheduleUnlink(_decoderQueue);
#endif
}

void
VideoReceiver::startRecording(const QString& videoFilePath, FILE_FORMAT format)
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "Starting recording";

    // exit immediately if we are already recording
    if (_pipeline == nullptr || _recording) {
        qCDebug(VideoReceiverLog) << "Already recording!";
        return;
    }

    _videoFile = videoFilePath;

    qCDebug(VideoReceiverLog) << "New video file:" << _videoFile;

    emit videoFileChanged();

    if ((_fileSink = _makeFileSink(_videoFile, format)) == nullptr) {
        qCCritical(VideoReceiverLog) << "_makeFileSink() failed";
        return;
    }

    _removingRecorder = false;

    gst_object_ref(_fileSink);

    gst_bin_add(GST_BIN(_pipeline), _fileSink);

    if (!gst_element_link(_recorderQueue, _fileSink)) {
        qCCritical(VideoReceiverLog) << "Failed to link queue and file sink";
        return;
    }

    gst_element_sync_state_with_parent(_fileSink);

    // Install a probe on the recording branch to drop buffers until we hit our first keyframe
    // When we hit our first keyframe, we can offset the timestamps appropriately according to the first keyframe time
    // This will ensure the first frame is a keyframe at t=0, and decoding can begin immediately on playback
    GstPad* probepad = gst_element_get_static_pad(_recorderQueue, "src");

    if (probepad == nullptr) {
        qCCritical(VideoReceiverLog) << "gst_element_get_static_pad() failed";
        return;
    }

    gst_pad_add_probe(probepad, GST_PAD_PROBE_TYPE_BUFFER, _keyframeWatch, this, nullptr); // to drop the buffers until key frame is received
    gst_object_unref(probepad);
    probepad = nullptr;

    _recording = true;

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-recording-start");

    emit recordingChanged();

    qCDebug(VideoReceiverLog) << "Recording started";
#else
    Q_UNUSED(videoFilePath)
    Q_UNUSED(format)
#endif
}

//-----------------------------------------------------------------------------
void
VideoReceiver::stopRecording(void)
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "Stopping recording";

    // exit immediately if we are not recording
    if (_pipeline == nullptr || !_recording) {
        qCDebug(VideoReceiverLog) << "Not recording!";
        return;
    }

    _removingRecorder = true;

    _scheduleUnlink(_recorderQueue);
#endif
}

void
VideoReceiver::grabImage(QString imageFile)
{
    _imageFile = imageFile;
#if defined(QGC_GST_STREAMING)
    // FIXME: AV: schedule screenshot taking here
#endif
    emit imageFileChanged();
}

#if defined(QGC_GST_STREAMING)
const char* VideoReceiver::_kFileMux[FILE_FORMAT_MAX - FILE_FORMAT_MIN] = {
    "matroskamux",
    "qtmux",
    "mp4mux"
};

void
VideoReceiver::_updateTimer(void)
{
    if(_stopping || _starting) {
        return;
    }
#if 0
    if(_streaming) {
        if(!_videoRunning) {
            _videoRunning = true;
            emit videoRunningChanged();
        }
    } else {
        if(_videoRunning) {
            _videoRunning = false;
            emit videoRunningChanged();
        }
    }

    if(_videoRunning) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();

        if(now - _lastFrameTime > _timeout) {
            stop();
            // We want to start it back again with _updateTimer
            _stop = false;
        }
    } else {
        // FIXME: AV: if pipeline is _running but not _streaming for some time then we need to restart
        if(!_stop && !_running && !_uri.isEmpty() && _streamEnabled) {
            start();
        }
    }
#endif
}

void
VideoReceiver::_handleError(void)
{
    qCDebug(VideoReceiverLog) << "Gstreamer error!";
    stop();
    _restart_timer.start(_restart_time_ms);
}

void
VideoReceiver::_handleEOS(void)
{
    if(_stopping) {
        if(_decoding && _removingDecoder) {
            _shutdownDecodingBranch();
        }
        if(_recording && _removingRecorder) {
            _shutdownRecordingBranch();
        }
        _shutdownPipeline();
        qCDebug(VideoReceiverLog) << "Stopped";
    } else if(_decoding && _removingDecoder) {
        _shutdownDecodingBranch();
    } else if(_recording && _removingRecorder) {
        _shutdownRecordingBranch();
    } else {
        qCWarning(VideoReceiverLog) << "Unexpected EOS!";
        _handleError();
    }
}

void
VideoReceiver::_handleStateChanged(void)
{
    if(_pipeline) {
        //_streaming = GST_STATE(_pipeline) == GST_STATE_PLAYING;
        //qCDebug(VideoReceiverLog) << "State changed, _streaming:" << _streaming;
    }
}

GstElement*
VideoReceiver::_makeSource(const QString& uri)
{
    if (uri.isEmpty()) {
        qCCritical(VideoReceiverLog) << "Failed because URI is not specified";
        return nullptr;
    }

    bool isTaisync  = uri.contains("tsusb://");
    bool isUdp264   = uri.contains("udp://");
    bool isRtsp     = uri.contains("rtsp://");
    bool isUdp265   = uri.contains("udp265://");
    bool isTcpMPEGTS= uri.contains("tcp://");
    bool isUdpMPEGTS= uri.contains("mpegts://");

    GstElement* source  = nullptr;
    GstElement* buffer  = nullptr;
    GstElement* parser  = nullptr;
    GstElement* bin     = nullptr;
    GstElement* srcbin  = nullptr;

    do {
        QUrl url(uri);

        if(isTcpMPEGTS) {
            if ((source = gst_element_factory_make("tcpclientsrc", "source")) != nullptr) {
                g_object_set(static_cast<gpointer>(source), "host", qPrintable(url.host()), "port", url.port(), nullptr);
            }
        } else if (isRtsp) {
            if ((source = gst_element_factory_make("rtspsrc", "source")) != nullptr) {
                g_object_set(static_cast<gpointer>(source), "location", qPrintable(uri), "latency", 17, "udp-reconnect", 1, "timeout", _udpReconnect_us, NULL);
            }
        } else if(isUdp264 || isUdp265 || isUdpMPEGTS || isTaisync) {
            if ((source = gst_element_factory_make("udpsrc", "source")) != nullptr) {
                g_object_set(static_cast<gpointer>(source), "uri", QString("udp://%1:%2").arg(qPrintable(url.host()), QString::number(url.port())).toUtf8().data(), nullptr);

                GstCaps* caps = nullptr;

                if(isUdp264) {
                    if ((caps = gst_caps_from_string("application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264")) == nullptr) {
                        qCCritical(VideoReceiverLog) << "gst_caps_from_string() failed";
                        break;
                    }
                } else if (isUdp265) {
                    if ((caps = gst_caps_from_string("application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H265")) == nullptr) {
                        qCCritical(VideoReceiverLog) << "gst_caps_from_string() failed";
                        break;
                    }
                }

                if (caps != nullptr) {
                    g_object_set(static_cast<gpointer>(source), "caps", caps, nullptr);
                    gst_caps_unref(caps);
                    caps = nullptr;
                }
            }
        } else {
            qCDebug(VideoReceiverLog) << "URI is not recognized";
        }

        if (!source) {
            qCCritical(VideoReceiverLog) << "gst_element_factory_make() for data source failed";
            break;
        }

        // FIXME: AV: Android does not determine MPEG2-TS via parsebin - have to explicitly state which demux to use
        if (isTcpMPEGTS || isUdpMPEGTS) {
            if ((parser = gst_element_factory_make("tsdemux", "parser")) == nullptr) {
                qCCritical(VideoReceiverLog) << "gst_element_factory_make('tsdemux') failed";
                break;
            }
        } else {
            if ((parser = gst_element_factory_make("parsebin", "parser")) == nullptr) {
                qCCritical(VideoReceiverLog) << "gst_element_factory_make('parsebin') failed";
                break;
            }
        }

        if ((bin = gst_bin_new("sourcebin")) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_bin_new('sourcebin') failed";
            break;
        }

        gst_bin_add_many(GST_BIN(bin), source, parser, nullptr);

        int probeRes = 0;

        gst_element_foreach_src_pad(source, _padProbe, &probeRes);

        if (probeRes & 1) {
            if (probeRes & 2) {
                if ((buffer = gst_element_factory_make("rtpjitterbuffer", nullptr)) == nullptr) {
                    qCCritical(VideoReceiverLog) << "gst_element_factory_make('rtpjitterbuffer') failed";
                    break;
                }

                gst_bin_add(GST_BIN(bin), buffer);

                if (!gst_element_link_many(source, buffer, parser, nullptr)) {
                    qCCritical(VideoReceiverLog) << "gst_element_link() failed";
                    break;
                }
            } else {
                if (!gst_element_link(source, parser)) {
                    qCCritical(VideoReceiverLog) << "gst_element_link() failed";
                    break;
                }
            }
        } else {
            g_signal_connect(source, "pad-added", G_CALLBACK(_linkPadWithOptionalBuffer), parser);
        }

        g_signal_connect(parser, "pad-added", G_CALLBACK(_wrapWithGhostPad), nullptr);

        source = buffer = parser = nullptr;

        srcbin = bin;
        bin = nullptr;
    } while(0);

    if (bin != nullptr) {
        gst_object_unref(bin);
        bin = nullptr;
    }

    if (parser != nullptr) {
        gst_object_unref(parser);
        parser = nullptr;
    }

    if (buffer != nullptr) {
        gst_object_unref(buffer);
        buffer = nullptr;
    }

    if (source != nullptr) {
        gst_object_unref(source);
        source = nullptr;
    }

    return srcbin;
}

GstElement*
VideoReceiver::_makeDecoder(GstCaps* caps, GstElement* videoSink)
{
    GstElement* decoder = nullptr;

    do {
        if ((decoder = gst_element_factory_make("decodebin", nullptr)) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_element_factory_make('decodebin') failed";
            break;
        }

        g_signal_connect(decoder, "autoplug-query", G_CALLBACK(_autoplugQuery), videoSink);
    } while(0);

    return decoder;
}

GstElement*
VideoReceiver::_makeFileSink(const QString& videoFile, FILE_FORMAT format)
{
    GstElement* fileSink = nullptr;
    GstElement* mux = nullptr;
    GstElement* sink = nullptr;
    GstElement* bin = nullptr;
    bool releaseElements = true;

    do{
        if (format < FILE_FORMAT_MIN || format >= FILE_FORMAT_MAX) {
            qCCritical(VideoReceiverLog) << "Unsupported file format";
            break;
        }

        if ((mux = gst_element_factory_make(_kFileMux[format - FILE_FORMAT_MIN], nullptr)) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_element_factory_make('" << _kFileMux[format - FILE_FORMAT_MIN] << "') failed";
            break;
        }

        if ((sink = gst_element_factory_make("filesink", nullptr)) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_element_factory_make('filesink') failed";
            break;
        }

        g_object_set(static_cast<gpointer>(sink), "location", qPrintable(videoFile), nullptr);

        if ((bin = gst_bin_new("sinkbin")) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_bin_new('sinkbin') failed";
            break;
        }

        GstPadTemplate* padTemplate;

        if ((padTemplate = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(mux), "video_%u")) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_element_class_get_pad_template(mux) failed";
            break;
        }

        // FIXME: AV: pad handling is potentially leaking (and other similar places too!)
        GstPad* pad;

        if ((pad = gst_element_request_pad(mux, padTemplate, nullptr, nullptr)) == nullptr) {
            qCCritical(VideoReceiverLog) << "gst_element_request_pad(mux) failed";
            break;
        }

        gst_bin_add_many(GST_BIN(bin), mux, sink, nullptr);

        releaseElements = false;

        GstPad* ghostpad = gst_ghost_pad_new("sink", pad);

        gst_element_add_pad(bin, ghostpad);

        gst_object_unref(pad);
        pad = nullptr;

        if (!gst_element_link(mux, sink)) {
            qCCritical(VideoReceiverLog) << "gst_element_link() failed";
            break;
        }

        fileSink = bin;
        bin = nullptr;
    } while(0);

    if (releaseElements) {
        if (sink != nullptr) {
            gst_object_unref(sink);
            sink = nullptr;
        }

        if (mux != nullptr) {
            gst_object_unref(mux);
            mux = nullptr;
        }
    }

    if (bin != nullptr) {
        gst_object_unref(bin);
        bin = nullptr;
    }

    return fileSink;
}

void
VideoReceiver::_onNewSourcePad(GstPad* pad)
{
    // FIXME: check for caps - if this is not video stream (and preferably - one of these which we have to support) then simply skip it
    if(!gst_element_link(_source, _tee)) {
        qCCritical(VideoReceiverLog) << "Unable to link source";
        return;
    }

    _streaming = true;

    if (_videoSink == nullptr) {
        return;
    }

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-new-source-pad");

    // FIXME: code below should be part of _addDecoder()
    GstPad* srcpad;

    if ((srcpad = gst_element_get_static_pad(_decoderQueue, "src")) == nullptr) {
        qCCritical(VideoReceiverLog) << "gst_element_get_static_pad() failed";
        return;
    }

    _decoding = _addDecoder(srcpad);

    gst_object_unref(srcpad);
    srcpad = nullptr;

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-decoding-start");

    if (!_decoding) {
        qCCritical(VideoReceiverLog) << "_addDecoder() failed";
        return;
    }

    qCDebug(VideoReceiverLog) << "Decoding started";
}

void
VideoReceiver::_onNewDecoderPad(GstPad* pad)
{
    if (!_addVideoSink(pad)) {
        qCCritical(VideoReceiverLog) << "_addVideoSink() failed";
    }

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-new-decoder-pad");
}

bool
VideoReceiver::_addDecoder(GstPad* pad)
{
    GstCaps* caps = gst_pad_query_caps(pad, nullptr);

    if (!caps) {
        qCCritical(VideoReceiverLog) << "gst_pad_query_caps() failed";
        return false;
    }

    if ((_decoder = _makeDecoder(caps, _videoSink)) == nullptr) {
        qCCritical(VideoReceiverLog) << "_makeDecoder() failed";
        gst_caps_unref(caps);
        caps = nullptr;
        return false;
    }

    gst_caps_unref(caps);
    caps = nullptr;

    // FIXME: AV: check if srcpad exists - if it does then no need to wait for new pad
    //    int probeRes = 0;
    //    gst_element_foreach_src_pad(source, _padProbe, &probeRes);
    g_signal_connect(_decoder, "pad-added", G_CALLBACK(_onNewPad), this);

    gst_bin_add(GST_BIN(_pipeline), _decoder);

    gst_element_sync_state_with_parent(_decoder);

    if (!gst_element_link(_decoderQueue, _decoder)) {
        qCCritical(VideoReceiverLog) << "Unable to link decoder";
        return false;
    }

    return true;
}

bool
VideoReceiver::_addVideoSink(GstPad* pad)
{
    GstCaps* caps = gst_pad_query_caps(pad, nullptr);

    gst_bin_add(GST_BIN(_pipeline), _videoSink);

    gst_element_sync_state_with_parent(_videoSink);

    if(!gst_element_link(_decoder, _videoSink)) {
        gst_bin_remove(GST_BIN(_pipeline), _videoSink);
        qCCritical(VideoReceiverLog) << "Unable to link video sink";
        if (caps != nullptr) {
            gst_caps_unref(caps);
            caps = nullptr;
        }
        return false;
    }

    if (caps != nullptr) {
        GstStructure* s = gst_caps_get_structure(caps, 0);

        if (s != nullptr) {
            gint width, height;
            gst_structure_get_int(s, "width", &width);
            gst_structure_get_int(s, "height", &height);
            _setVideoSize(QSize(width, height));
        }

        gst_caps_unref(caps);
        caps = nullptr;
    } else {
        _setVideoSize(QSize(0, 0));
    }

    // FIXME: AV: don't we signal decoding somewhere else?
    _decoding = true;
    emit decodingChanged();

    return true;
}

void
VideoReceiver::_noteVideoSinkFrame(void)
{
    _lastFrameTime = QDateTime::currentSecsSinceEpoch();
}

void
VideoReceiver::_scheduleUnlink(GstElement* from)
{
    GstPad* pad;

    if ((pad = gst_element_get_static_pad(from, "src")) == nullptr) {
        qCCritical(VideoReceiverLog) << "gst_element_get_static_pad() failed";
        return;
    }

    // Wait for data block before unlinking
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_IDLE, _unlinkBranch, this, nullptr);
    gst_object_unref(pad);
    pad = nullptr;
}

// -Unlink the branch from the src pad
// -Send an EOS event at the beginning of that branch
void
VideoReceiver::_unlinkBranch(GstPad* src)
{
    GstPad* sink;

    if ((sink = gst_pad_get_peer(src)) == nullptr) {
        qCCritical(VideoReceiverLog) << "gst_pad_get_peer() failed";
        return;
    }

    gst_pad_unlink(src, sink);

    // Send EOS at the beginning of the branch
    gst_pad_send_event(sink, gst_event_new_eos());

    gst_object_unref(sink);
    sink = nullptr;

    qCDebug(VideoReceiverLog) << "Branch EOS was sent";
}

void
VideoReceiver::_shutdownDecodingBranch(void)
{
    gst_bin_remove(GST_BIN(_pipeline), _decoder);
    _decoder = nullptr;

    gst_bin_remove(GST_BIN(_pipeline), _videoSink);
    gst_element_set_state(_videoSink, GST_STATE_NULL);
    gst_object_unref(_videoSink);
    _videoSink = nullptr;

    _decoding = false;

    emit decodingChanged();

    qCDebug(VideoReceiverLog) << "Decoding stopped";

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-decoding-stopped");
}

void
VideoReceiver::_shutdownRecordingBranch(void)
{
    gst_bin_remove(GST_BIN(_pipeline), _fileSink);
    gst_element_set_state(_fileSink, GST_STATE_NULL);
    gst_object_unref(_fileSink);
    _fileSink = nullptr;

    _recording = false;

    emit recordingChanged();

    qCDebug(VideoReceiverLog) << "Recording stopped";

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-recording-stopped");
}

void
VideoReceiver::_shutdownPipeline()
{
    if(!_pipeline) {
        qCDebug(VideoReceiverLog) << "No pipeline";
        return;
    }
    GstBus* bus = nullptr;
    if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != nullptr) {
        gst_bus_disable_sync_message_emission(bus);
        gst_object_unref(bus);
        bus = nullptr;
    }
    gst_element_set_state(_pipeline, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(_pipeline), _videoSink);
    // FIXME: rework this crap
    _decoding = false;
    _decoder = nullptr;
    _decoderQueue = nullptr;
    _tee = nullptr;
    _source = nullptr;
    gst_object_unref(_pipeline);
    _pipeline = nullptr;
    _streaming = false;
    _recording = false;
    _stopping = false;
    _running = false;
    emit recordingChanged();
}

gboolean
VideoReceiver::_onBusMessage(GstBus* bus, GstMessage* msg, gpointer data)
{
    Q_UNUSED(bus)
    Q_ASSERT(msg != nullptr && data != nullptr);
    VideoReceiver* pThis = (VideoReceiver*)data;

    switch(GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
        do {
            gchar* debug;
            GError* error;
            gst_message_parse_error(msg, &error, &debug);
            g_free(debug);
            qCCritical(VideoReceiverLog) << error->message;
            g_error_free(error);
            pThis->_handleError();
        } while(0);
        break;
    case GST_MESSAGE_EOS:
        pThis->_handleEOS();
        break;
    case GST_MESSAGE_STATE_CHANGED:
        pThis->_handleStateChanged();
        break;
    case GST_MESSAGE_ELEMENT:
        do {
            const GstStructure* s = gst_message_get_structure (msg);

            if (!gst_structure_has_name (s, "GstBinForwarded")) {
                break;
            }

            GstMessage* forward_msg = nullptr;

            gst_structure_get(s, "message", GST_TYPE_MESSAGE, &forward_msg, NULL);

            if (forward_msg == nullptr) {
                break;
            }

            if (GST_MESSAGE_TYPE(forward_msg) == GST_MESSAGE_EOS) {
                pThis->_handleEOS();
            }

            gst_message_unref(forward_msg);
            forward_msg = nullptr;
        } while(0);
        break;
    default:
        break;
    }

    return TRUE;
}

void
VideoReceiver::_onNewPad(GstElement* element, GstPad* pad, gpointer data)
{
    VideoReceiver* self = static_cast<VideoReceiver*>(data);

    if (element == self->_source) {
        self->_onNewSourcePad(pad);
    } else if (element == self->_decoder) {
        self->_onNewDecoderPad(pad);
    } else {
        qCDebug(VideoReceiverLog) << "Unexpected call!";
    }
}

void
VideoReceiver::_wrapWithGhostPad(GstElement* element, GstPad* pad, gpointer data)
{
    gchar* name = gst_pad_get_name(pad);

    GstPad* ghostpad = gst_ghost_pad_new(name, pad);

    g_free(name);

    gst_pad_set_active(ghostpad, TRUE);

    if (!gst_element_add_pad(GST_ELEMENT_PARENT(element), ghostpad)) {
        qCCritical(VideoReceiverLog) << "Failed to add ghost pad to source";
    }
}

void
VideoReceiver::_linkPadWithOptionalBuffer(GstElement* element, GstPad* pad, gpointer data)
{
    gboolean isRtpPad = FALSE;

    GstCaps* filter = gst_caps_from_string("application/x-rtp");

    if (filter != nullptr) {
        GstCaps* caps = gst_pad_query_caps(pad, nullptr);

        if (caps != nullptr) {
            if (!gst_caps_is_any(caps) && gst_caps_can_intersect(caps, filter)) {
                isRtpPad = TRUE;
            }
            gst_caps_unref(caps);
            caps = nullptr;
        }

        gst_caps_unref(filter);
        filter = nullptr;
    }

    if (isRtpPad) {
        GstElement* buffer;

        if ((buffer = gst_element_factory_make("rtpjitterbuffer", nullptr)) != nullptr) {
            gst_bin_add(GST_BIN(GST_ELEMENT_PARENT(element)), buffer);

            gst_element_sync_state_with_parent(buffer);

            GstPad* sinkpad = gst_element_get_static_pad(buffer, "sink");

            if (sinkpad != nullptr) {
                const GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);

                gst_object_unref(sinkpad);
                sinkpad = nullptr;

                if (ret == GST_PAD_LINK_OK) {
                    pad = gst_element_get_static_pad(buffer, "src");
                    element = buffer;
                } else {
                    qCDebug(VideoReceiverLog) << "Partially failed - gst_pad_link()";
                }
            } else {
                qCDebug(VideoReceiverLog) << "Partially failed - gst_element_get_static_pad()";
            }
        } else {
            qCDebug(VideoReceiverLog) << "Partially failed - gst_element_factory_make('rtpjitterbuffer')";
        }
    }

    gchar* name = gst_pad_get_name(pad);

    if (name != nullptr) {
        if(gst_element_link_pads(element, name, GST_ELEMENT(data), "sink") == false) {
            qCCritical(VideoReceiverLog) << "Failed to link elements\n";
        }

        g_free(name);
        name = nullptr;
    } else {
        qCCritical(VideoReceiverLog) << "gst_pad_get_name() failed";
    }
}

gboolean
VideoReceiver::_padProbe(GstElement* element, GstPad* pad, gpointer user_data)
{
    int* probeRes = (int*)user_data;

    *probeRes |= 1;

    GstCaps* filter = gst_caps_from_string("application/x-rtp");

    if (filter != nullptr) {
        GstCaps* caps = gst_pad_query_caps(pad, nullptr);

        if (caps != nullptr) {
            if (!gst_caps_is_any(caps) && gst_caps_can_intersect(caps, filter)) {
                *probeRes |= 2;
            }

            gst_caps_unref(caps);
            caps = nullptr;
        }

        gst_caps_unref(filter);
        filter = nullptr;
    }

    return TRUE;
}

gboolean
VideoReceiver::_autoplugQueryCaps(GstElement* bin, GstPad* pad, GstElement* element, GstQuery* query, gpointer data)
{
    GstElement* glupload = (GstElement* )data;

    GstPad* sinkpad = gst_element_get_static_pad(glupload, "sink");

    if (!sinkpad) {
        qCCritical(VideoReceiverLog) << "No sink pad found";
        return FALSE;
    }

    GstCaps* filter;

    gst_query_parse_caps(query, &filter);

    GstCaps* sinkcaps = gst_pad_query_caps(sinkpad, filter);

    gst_query_set_caps_result(query, sinkcaps);

    const gboolean ret = !gst_caps_is_empty(sinkcaps);

    gst_caps_unref(sinkcaps);
    sinkcaps = nullptr;

    gst_object_unref(sinkpad);
    sinkpad = nullptr;

    return ret;
}

gboolean
VideoReceiver::_autoplugQueryContext(GstElement* bin, GstPad* pad, GstElement* element, GstQuery* query, gpointer data)
{
    GstElement* glsink = (GstElement* )data;

    GstPad* sinkpad = gst_element_get_static_pad(glsink, "sink");

    if (!sinkpad){
        qCCritical(VideoReceiverLog) << "No sink pad found";
        return FALSE;
    }

    const gboolean ret = gst_pad_query(sinkpad, query);

    gst_object_unref(sinkpad);
    sinkpad = nullptr;

    return ret;
}

gboolean
VideoReceiver::_autoplugQuery(GstElement* bin, GstPad* pad, GstElement* element, GstQuery* query, gpointer data)
{
    gboolean ret;

    switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
        ret = _autoplugQueryCaps(bin, pad, element, query, data);
        break;
    case GST_QUERY_CONTEXT:
        ret = _autoplugQueryContext(bin, pad, element, query, data);
        break;
    default:
        ret = FALSE;
        break;
    }

    return ret;
}

GstPadProbeReturn
VideoReceiver::_videoSinkProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    Q_UNUSED(pad);
    Q_UNUSED(info)

    if(user_data != nullptr) {
        VideoReceiver* pThis = static_cast<VideoReceiver*>(user_data);
        pThis->_noteVideoSinkFrame();
    }

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn
VideoReceiver::_keyframeWatch(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    if (info == nullptr || user_data == nullptr) {
        qCCritical(VideoReceiverLog) << "Invalid arhuments";
        return GST_PAD_PROBE_DROP;
    }

    GstBuffer* buf = gst_pad_probe_info_get_buffer(info);

    if (GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) { // wait for a keyframe
        return GST_PAD_PROBE_DROP;
    }

    // set media file '0' offset to current timeline position - we don't want to touch other elements in the graph, except these which are downstream!
    gst_pad_set_offset(pad, -static_cast<gint64>(buf->pts));

    VideoReceiver* pThis = static_cast<VideoReceiver*>(user_data);

    qCDebug(VideoReceiverLog) << "Got keyframe, stop dropping buffers";

    pThis->gotFirstRecordingKeyFrame();

    return GST_PAD_PROBE_REMOVE;
}

GstPadProbeReturn
VideoReceiver::_unlinkBranch(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    Q_UNUSED(info);

    Q_ASSERT(user_data != nullptr);

    VideoReceiver* pThis = static_cast<VideoReceiver*>(user_data);

    pThis->_unlinkBranch(pad);

    return GST_PAD_PROBE_REMOVE;
}
#endif
