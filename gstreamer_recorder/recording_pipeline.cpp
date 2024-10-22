#include <gst/gst.h>
#include <glib.h>
#include <thread>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <unistd.h> 
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <glog/logging.h>

/* variable to keep track of ctr+c cout to stop the program in case of program getting hanged */
volatile sig_atomic_t ctrl_c_count = 0;

/* Variable to store the last known valid PTS from depay */
static GstClockTime last_valid_pts = GST_CLOCK_TIME_NONE;

/* Running Configurations */
static const gchar *mac = "MAC";
static guint stream_enc = 0; // Default: H264
static guint chunk_size = 300;

GOptionEntry entries[] = {
{"mac", 'm', 0, G_OPTION_ARG_STRING, &mac,
    "mac", NULL}
,   
{"stream-enc", 'h', 0, G_OPTION_ARG_INT, &stream_enc,
      "Stream type: 0 = H264, \
       1 = H265, \
       Default: H264", NULL}
,
{"record-chunk", 't', 0, G_OPTION_ARG_INT, &chunk_size,
    "Stream record chunk size, \
    In seconds, \
    Default: 300 sec", NULL}
};

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData {
    GstElement *rtspsrc;
    GstElement *capsfilter;
    GstElement *queue;
    GstElement *depay;
    GstElement *parse;
} CustomData;

/* Following element pointers are globaly defined to access them within the signal handler */
static GstElement *pipeline = NULL, *splitmuxsink = NULL;
static GMainLoop *loop = NULL;

/* Handler for the pad-added signal */
static void pad_added_handler (GstElement *src, GstPad *pad, CustomData *data);

/* Bus callback function */
static gboolean bus_call (GstBus * bus, GstMessage * msg, gpointer data);

/* Pipeline stop function after a certain time limit */
static void stop_pipeline_after_timeout (CustomData *data, guint timeout_seconds);

/* Signal handler function stop the pipeline upon the correct signal received */
static void signal_handler(int signum);

/* Rename recording chunk files to human readable format */
static gchararray rename_recording_chunk (GstElement * splitmux, guint fragment_id, const char* record_folder);

static guint64 convert_to_nano_seconds (guint original_value);

static gint createFolder(const char* folderPath);

int main(int argc, char *argv[]) {

    // Initialize Google’s logging library.
    google::InitGoogleLogging(argv[0]);
    google::SetLogDestination(google::ERROR, "/var/log/realtime/recording_pipeline_error_logs_");

    LOG(INFO) << "[Recorder] - Recorder Logging Initialized";

    CustomData data;
    GstBus *bus = NULL;
    GstMessage *msg;
    GstStateChangeReturn ret;
    gboolean terminate = FALSE;
    guint bus_watch_id = 0;
    GstCaps *caps = NULL; 

    GOptionContext *gctx = NULL;
    GOptionGroup *group = NULL;
    GError *error = NULL;

    gctx = g_option_context_new ("Gstreamer Recording app");
    group = g_option_group_new ("gstreamer_recorder", NULL, NULL, NULL, NULL);
    g_option_group_add_entries (group, entries);

    g_option_context_set_main_group (gctx, group);
    g_option_context_add_group (gctx, gst_init_get_option_group ());

    if (!g_option_context_parse (gctx, &argc, &argv, &error)) {
        LOG(ERROR) << "[Recorder] - " << error->message << "\n"; 
        LOG(ERROR) << "[Recorder]- " << g_option_context_get_help(gctx, TRUE, NULL) << "\n";
        return -1;
    }

    /* Initialize GStreamer */
    gst_init (&argc, &argv);

     /* Set up a GMainLoop */
    loop = g_main_loop_new(NULL, FALSE);

    /* Create the elements */
    data.rtspsrc = gst_element_factory_make("rtspsrc", "rtspsrc");
    data.capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    data.queue = gst_element_factory_make("queue", "queue");
    if (stream_enc == 0){
        data.depay = gst_element_factory_make("rtph264depay", "h264-depay");
        data.parse = gst_element_factory_make("h264parse", "h264-parse");
    }
    else {
        data.depay = gst_element_factory_make("rtph265depay", "h265-depay");
        data.parse = gst_element_factory_make("h265parse", "h265-parse");
    }
    splitmuxsink = gst_element_factory_make("splitmuxsink", "splitmuxsink");

    /* Create the empty pipeline */
    pipeline = gst_pipeline_new ("test-pipeline");

    if (!pipeline || !data.rtspsrc || !data.capsfilter || !data.queue || !data.depay || !data.parse || !splitmuxsink) {
            LOG(ERROR) << "[Recorder] - Not all elements could be created.\n";
            return -1;
    }

    /* Add elements to the pipeline */
    gst_bin_add_many(GST_BIN(pipeline), data.rtspsrc, data.capsfilter, data.queue, data.depay, data.parse, splitmuxsink, NULL);

    if (!gst_element_link_many(data.capsfilter, data.queue, data.depay, data.parse, splitmuxsink, NULL)) {
        LOG(ERROR) << "[Recorder] - Error linking elements.\n";
        gst_object_unref(pipeline);
        return -1;
    }

    /* Convert recording chunk size */
    guint64 result = convert_to_nano_seconds(chunk_size);
    if (result == -1){
        LOG(ERROR) << "[Recorder] - Numerical Conversion Error.\n";
        gst_object_unref (pipeline);
        return -1;
    }

    /* Create recording folders */
    char stream_record_folder [60];
    sprintf(stream_record_folder, "./recorded_streams");  
    createFolder(stream_record_folder);

    char camera_record_folder [90];
    sprintf(camera_record_folder, "%s/%s", stream_record_folder, mac);
    createFolder(camera_record_folder);


    /* Set splitmuxsink properties */ 
    g_object_set(data.rtspsrc, "location", argv[1], NULL);
    // g_object_set(data.filesink, "location", "received_h264.mkv", NULL);
    // g_object_set(splitmuxsink, "location", "recordings/received_h264_%05d.mp4", NULL);
    g_object_set(splitmuxsink, "max-size-time", result, NULL);  // Set max file size time (nanoseconds)
    // g_object_set(splitmuxsink, "muxer-factory", "mp4mux", NULL );
    // g_object_set(splitmuxsink, "async-finalize", true, NULL);

    /* Set caps properties */    
    if (stream_enc == 0){
        caps = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "video", "encoding-name", G_TYPE_STRING, "H264", NULL);
    }
    else {
        caps = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "video", "encoding-name", G_TYPE_STRING, "H265", NULL);
    }
    g_object_set(data.capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    /* pad added call back funtion */
    g_signal_connect (data.rtspsrc, "pad-added", G_CALLBACK (pad_added_handler), &data);
    /* add PTS fixing probe to the parser's src pad */
    add_pts_fix_probe(data.parse);
    /* rename file chunk call back funtion */
    g_signal_connect (splitmuxsink, "format-location", G_CALLBACK (rename_recording_chunk), camera_record_folder);

    signal(SIGINT, signal_handler);

    /* Start a thread to stop the pipeline after a certain time */
    // std::thread stopThread(stop_pipeline_after_timeout, &data, 25);  // 60 seconds, adjust as needed

    /* Listen to the bus */
    bus = gst_element_get_bus (pipeline);
    bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
    gst_object_unref (bus);

     /* Start playing */
    ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG(ERROR) << "[Recorder] - Unable to set the pipeline to the playing state.\n";
        gst_object_unref (pipeline);
        return -1;
    }

    /* Start the main loop */
    LOG(INFO) << "[Recorder] - main loop running. \n";
    g_main_loop_run(loop);

    LOG(INFO) << "[Recorder] - out of the main loop. \n";

    /* Free resources */
    // stopThread.join();
    g_main_loop_unref(loop);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
    return 0;
}

static gboolean bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      LOG(INFO) << "[Recorder] - End of stream. \n";
      g_main_loop_quit (loop);
      break;
    case GST_MESSAGE_ERROR:{
      gchar *debug;
      GError *error;
      gst_message_parse_error (msg, &error, &debug);
      LOG(ERROR) << "[Recorder] - ERROR from element " << GST_OBJECT_NAME(msg->src) << " : " << error->message << "\n";
      if (debug)
        LOG(ERROR) << "[Recorder] - Error details: " << debug << "\n";
      g_free (debug);
      g_error_free (error);
      g_main_loop_quit (loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

/* This function will be called by the pad-added signal */
static void pad_added_handler (GstElement *src, GstPad *new_pad, CustomData *data) {
    GstPad *sink_pad = gst_element_get_static_pad (data->capsfilter, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    LOG(INFO) << "[Recorder] - Received new pad " << GST_PAD_NAME (new_pad) << " from " << GST_ELEMENT_NAME (src) << "\n";

    /* If our converter is already linked, we have nothing to do here */
    if (gst_pad_is_linked (sink_pad)) {
        LOG(INFO) << "[Recorder] - We are already linked. Ignoring.\n";
        goto exit;
    }

    /* Check the new pad's type */
    new_pad_caps = gst_pad_get_current_caps (new_pad);
    new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
    new_pad_type = gst_structure_get_name (new_pad_struct);
    if (!g_str_has_prefix (new_pad_type, "application/x-rtp")) {
        LOG(INFO) << "[Recorder] - Ignoring pad with unexpected type: " << new_pad_type;
        goto exit;
    }

    /* Attempt the link */
    ret = gst_pad_link (new_pad, sink_pad);
    if (GST_PAD_LINK_FAILED (ret)) {
        LOG(INFO) << "[Recorder] - Failed to link pad with type: " << new_pad_type;
    } else {
        LOG(INFO) << "[Recorder] - Successfully linked pad with type: " << new_pad_type;
    }

exit:
    /* Unreference the new pad's caps, if we got them */
    if (new_pad_caps != NULL)
        gst_caps_unref (new_pad_caps);

    /* Unreference the sink pad */
    gst_object_unref (sink_pad);
}

/* Stops a GStreamer pipeline after a specified timeout */
static void stop_pipeline_after_timeout(CustomData *data, guint timeout_seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(timeout_seconds));

    // 1. Send EOS event to splitmuxsink
    GstEvent *eos_event = gst_event_new_eos();
    gst_element_send_event(splitmuxsink, eos_event);

    // 2. Wait for EOS message on the bus (optional but recommended)
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg;
    do {
        msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_EOS);
        if (msg != NULL) {
            gst_message_unref(msg);
            break; // EOS received, proceed to stop the pipeline
        }
    } while (true); 

    // 3. Set the pipeline to NULL state
    GstStateChangeReturn ret;
    GstState state;
    ret = gst_element_get_state(pipeline, &state, NULL, 5 * GST_SECOND); // 5-second timeout

    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG(ERROR) << "[Recorder] - Failed to set pipeline to NULL state.\n";
    } else {
        LOG(INFO) << "[Recorder] - Pipeline successfully stopped.\n";
    }

    LOG(INFO) << "[Recorder] - This does executes. \n";

    // 4. Quit the main loop
    g_main_loop_quit(loop);
}

/* Signal handler function to finish recording stream and stop the pipeline if a stopping signal is received */
static void signal_handler(int signum) {

    ++ctrl_c_count;

    /* Terminate the pipeline if 3 ctr+c commands received */
    if (ctrl_c_count >= 3) {
      LOG(INFO) << "[Recorder] - Ctrl+C pressed twice. Exiting immediately. \n";
      pid_t pid = getpid();
      int result = kill(pid, SIGTERM);
      if (!(result == 0)) {
          LOG(INFO) << "[Recorder] - Failed to send SIGTERM signal to terminate the current process. \n";
      }
    }

    // 1. Send EOS event to splitmuxsink
    GstEvent *eos_event = gst_event_new_eos();
    gst_element_send_event(splitmuxsink, eos_event);
}

/* Rename recording chunk files to human readable format */
static gchararray rename_recording_chunk (GstElement * splitmux, guint fragment_id, const char* record_folder) {

    auto now = std::chrono::system_clock::now();
    std::time_t time_now = std::chrono::system_clock::to_time_t(now);

    // Get time zone
    tm* local_time = std::localtime(&time_now);

    gchar* result_string = g_strdup_printf(
        "%s/stream_%05u_%04d%02d%02d-%02d%02d%02d_%s.mp4",
        record_folder, 
        fragment_id,
        local_time->tm_year + 1900, // tm_year is years since 1900
        local_time->tm_mon + 1,      // tm_mon is 0-based
        local_time->tm_mday,
        local_time->tm_hour,
        local_time->tm_min,
        local_time->tm_sec,
        local_time->tm_zone         // tm_zone directly holds the timezone string
    );

    LOG(INFO) << "[Recorder] - " << result_string;

    return result_string;
}

/* Helper function to convert user inserted seconds time value to nano-seconds */
static guint64 convert_to_nano_seconds (guint original_value) {
    guint64 large_value = static_cast<guint64>(original_value);

    if (large_value > (std::numeric_limits<guint64>::max() / 1e9)) {
        LOG(ERROR) << "[Recorder] - Error: Multiplication would cause overflow!\n";
        return -1; 
    }

    return large_value * 1e9; 
}

static gint createFolder(const char* folderPath) {
    struct stat st = {0};

    if (stat(folderPath, &st) == 0) {
        return 0; 
    }

    if (mkdir(folderPath, 0766) == 0) {
        return 1; // Success
    } else {
        // Check if the failure was due to the folder already existing
        if (errno == EEXIST) {
            return 0; 
        } else {
            return -1; 
        }
    }
}

/* Function to add a probe and fix missing PTS */
static GstPadProbeReturn fix_missing_pts(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstClockTime pts = GST_BUFFER_PTS(buffer);

    if (pts == GST_CLOCK_TIME_NONE) {
        /* set it to the last known valid PTS from depay */
        GST_BUFFER_PTS(buffer) = last_valid_pts;
        LOG(INFO) << "[PTS Fix] - Missing PTS, using last known valid PTS: " << last_valid_pts << "\n";
    } else {
        /* Update the last valid PTS if present */
        last_valid_pts = pts;
    }

    return GST_PAD_PROBE_OK;
}

/* Function to add the pad probe to the parser's src pad */
static void add_pts_fix_probe(GstElement *parser) {
    GstPad *parser_src_pad = gst_element_get_static_pad(parser, "src");
    gst_pad_add_probe(parser_src_pad, GST_PAD_PROBE_TYPE_BUFFER, fix_missing_pts, NULL, NULL);
    gst_object_unref(parser_src_pad);
}
