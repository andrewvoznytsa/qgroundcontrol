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
 *   @brief QGC's builtin GstBuffer metadata manipulation plugin
 *   @author Andrew Voznytsa <andrew.voznytsa@gmail.com>
 */

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

#define GST_CAT_DEFAULT gst_qgc_metamagic_debug

GST_DEBUG_CATEGORY(GST_CAT_DEFAULT);

typedef struct _GstQgcMetamagic GstQgcMetamagic;
typedef struct _GstQgcMetamagicClass GstQgcMetamagicClass;

enum {
    PROP_0,
    PROP_TIMESTAMP_SHIFT,
};

#define DEFAULT_TIMESTAMP_SHIFT 0

#define GST_TYPE_QGC_METAMAGIC (gst_qgc_metamagic_get_type())
#define GST_QGC_METAMAGIC(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_QGC_METAMAGIC, GstQgcMetamagic))
#define GST_QGC_METAMAGIC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_QGC_METAMAGIC, GstQgcMetamagicClass))
#define GST_QGC_METAMAGIC_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_QGC_METAMAGIC, GstQgcMetamagicClass))
#define GST_IS_QGC_METAMAGIC(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_QGC_METAMAGIC))
#define GST_IS_QGC_METAMAGIC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_QGC_METAMAGIC))
#define GST_QGC_METAMAGIC_CAST(obj) ((GstQgcMetamagic* )(obj))

struct _GstQgcMetamagicClass {
    GstBaseTransformClass parent_class;
};

struct _GstQgcMetamagic {
    GstBaseTransform base_trans;
    gint64 timestamp_shift;
};

static GstStaticPadTemplate _mm_sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate _mm_src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

#define gst_qgc_metamagic_parent_class parent_class

G_DEFINE_TYPE(GstQgcMetamagic, gst_qgc_metamagic, GST_TYPE_BASE_TRANSFORM);

static void
_mm_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec)
{
    GstQgcMetamagic* mm = GST_QGC_METAMAGIC(object);

    switch (prop_id) {
    case PROP_TIMESTAMP_SHIFT:
        mm->timestamp_shift = g_value_get_int64(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
_mm_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec)
{
    GstQgcMetamagic* mm = GST_QGC_METAMAGIC(object);

    switch (prop_id) {
    case PROP_TIMESTAMP_SHIFT:
        g_value_set_int64(value, mm->timestamp_shift);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static GstFlowReturn
_mm_prepare_output_buffer(GstBaseTransform* trans, GstBuffer* inbuf, GstBuffer** outbuf)
{
    GstQgcMetamagic* mm = GST_QGC_METAMAGIC(trans);

    if (mm->timestamp_shift == 0) {
        *outbuf = inbuf;
        return GST_FLOW_OK;
    }

    if (gst_buffer_is_writable(inbuf)) {
        *outbuf = inbuf;
    } else {
        if((*outbuf = gst_buffer_copy(inbuf)) == NULL) {
            return GST_FLOW_ERROR;
        }
    }

    GstBuffer* buf = *outbuf;

    if (buf->pts != GST_CLOCK_TIME_NONE) {
        buf->pts += mm->timestamp_shift;
    }

    if (buf->dts != GST_CLOCK_TIME_NONE) {
        buf->dts += mm->timestamp_shift;
    }

    return GST_FLOW_OK;
}

static void
gst_qgc_metamagic_class_init(GstQgcMetamagicClass* klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;
    GstBaseTransformClass *gstbasetransform_class;
    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *) klass;
    gstbasetransform_class = (GstBaseTransformClass *) klass;

    /* Overide base class functions */
    gobject_class->set_property = GST_DEBUG_FUNCPTR(_mm_set_property);
    gobject_class->get_property = GST_DEBUG_FUNCPTR(_mm_get_property);

    gstbasetransform_class->prepare_output_buffer = GST_DEBUG_FUNCPTR(_mm_prepare_output_buffer);
    gstbasetransform_class->passthrough_on_same_caps = TRUE;

    /* Install properties */
    g_object_class_install_property (gobject_class, PROP_TIMESTAMP_SHIFT,
        g_param_spec_int64("timestamp-shift", "Timestamp shift", "Timestamp shift for every GstBuffer",
            G_MININT64, G_MAXINT64, DEFAULT_TIMESTAMP_SHIFT, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    /* Set sink and src pad capabilities */
    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&_mm_sink_template));
    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&_mm_src_template));

    /* Set metadata describing the element */
    gst_element_class_set_details_simple(gstelement_class,
          "qgcmetamagic",
          "qgcmetamagic",
          "GStreamer plugin for QGC's Video Receiver",
          "See http://qgroundcontrol.com/");
}

static void
gst_qgc_metamagic_init(GstQgcMetamagic* mm)
{
    mm->timestamp_shift = 0;
}

static gboolean
plugin_init(GstPlugin* plugin)
{
    if (!gst_element_register(plugin, "qgcmetamagic", GST_RANK_NONE, GST_TYPE_QGC_METAMAGIC)) {
        return FALSE;
    }

    return TRUE;
}

#define PACKAGE            "QGC Video Receiver"
#define PACKAGE_VERSION    "current"
#define GST_LICENSE        "LGPL"
#define GST_PACKAGE_NAME   "GStreamer plugin for QGC's Video Receiver"
#define GST_PACKAGE_ORIGIN "http://qgroundcontrol.com/"

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    qgc, "QGC Video Receiver plugin",
    plugin_init, PACKAGE_VERSION,
    GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
