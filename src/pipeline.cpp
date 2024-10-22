/*
 * Copyright (c) 2020-2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "pipeline.h"
#include <glog/logging.h>
#include <json.hpp>

GST_DEBUG_CATEGORY (NVDS_APP);
int frame_num = 0;

// alarm delaying constants
const std::chrono::seconds BASE_INTERVAL = std::chrono::seconds(60);
const std::chrono::seconds MAX_INTERVAL = std::chrono::seconds(420); // 7 minutes
const int RECORDING_FREQUENCY_THRESHOLD = 2; // Only this amount of videos will be recorded in a given interval
std::chrono::seconds current_interval = BASE_INTERVAL;
int recording_counter = 0;
std::chrono::system_clock::time_point last_alarm_generated;


char const *tracker_config_file;
volatile sig_atomic_t ctrl_c_count = 0;
bool is_record_stopped = false;
AmqpClient::Channel::ptr_t channel;

gchar file_name_prefix[] = "incident";
gchar stream_name_prefix[] = "stream";

/* Running Configurations */
static gboolean bbox_enabled = 0;
static gboolean is_recording = 0;
static gint enc_type = 1; // Default: Software encoder
static gint sink_type = 3; // Default: Eglsink
static guint sr_mode = 1; // Default: Audio + Video
static guint pgie_type = 0; // Default: Nvinfer
static guint stream_enc = 0; // Default: H264
static guint camera_id = 0;
static gchar *mac = NULL;
static guint port = 8554; 
static guint running_mode = 1; // 1: fakesink & incident 2: udp sink & incidents
static guint motion = 1;
static guint debug_level = 0;
static guint chunk_size = STREAM_REC_DEFAULT_DURATION; // Default: 10800 Secs
static gboolean person_detection_enabled = IS_PERSON_DETECTION_ENABLED;
static gboolean vehicle_detection_enabled = IS_VEHICLE_DETECTION_ENABLED;

const int vehicle_class_ids[] = PGIE_CLASS_ID_VEHICLE;
const int PGIE_CLASS_IDS_SIZE = sizeof(vehicle_class_ids) / sizeof(vehicle_class_ids[0]);

GOptionEntry entries[] = {
  {"bbox-enable", 'e', 0, G_OPTION_ARG_INT, &bbox_enabled,
      "0: Disable bboxes, \
       1: Enable bboxes, \
       Default: bboxes disabled", NULL}
  ,
  {"enc-type", 'c', 1, G_OPTION_ARG_INT, &enc_type,
      "0: Hardware encoder, \
       1: Software encoder, \
       Default: Software encoder", NULL}
  ,
  {"sink-type", 's', 3, G_OPTION_ARG_INT, &sink_type,
      "1: Fakesink, \
       2: Eglsink, \
       3: RTSP sink, \
       Default: RTSP sink", NULL}
  ,
  {"sr-mode", 'm', 0, G_OPTION_ARG_INT, &sr_mode,
      "SR mode: 0 = Audio + Video, \
       1 = Video only, \
       2 = Audio only", NULL}
  ,
  {"pgie-type", 'p', 0, G_OPTION_ARG_INT, &pgie_type,
      "PGIE type: 0 = Nvinfer, \
       1 = Nvinferserver, \
       Default: Nvinfer", NULL}
  ,
  {"stream-enc", 'h', 0, G_OPTION_ARG_INT, &stream_enc,
      "Streram type: 0 = H264, \
       1 = H265, \
       Default: H264", NULL}
  ,
  {"camera-id", 'i', 0, G_OPTION_ARG_INT, &camera_id,
      "Camera ID as an int", NULL}
  ,
  {"mac", 'm', 0, G_OPTION_ARG_STRING, &mac,
      "mac", NULL}
  ,
  {"port", 'r', 0, G_OPTION_ARG_INT, &port,
      "RTSP output port", NULL}
  ,
  {"running-mode", 's', 0, G_OPTION_ARG_INT, &running_mode,
      "1: No RTSP output, \
       2: Processed RTSP output, \
       Default: No RTSP Output", NULL}
  ,
  {"motion", 'o', 0, G_OPTION_ARG_INT, &motion,
      "0: Disabled, \
       1: Enabled, \
       Default: Disabled", NULL}
  ,
  {"stream-record", 'a', 0, G_OPTION_ARG_INT, &is_recording,
    "0: Disable stream recording, \
      1: Enable stream recording, \
      Default: stream record disabled", NULL}
  ,
  {"record-chunk", 't', 0, G_OPTION_ARG_INT, &chunk_size,
      "Stream record chunk size, \
       In seconds, \
       Default: 10800 sec", NULL}
  ,
  {"person-detection", 'n', 0, G_OPTION_ARG_INT, &person_detection_enabled,
    "0: Disable person detection, \
      1: Enable person detection, \
      Default: person detection enabled", NULL}
  ,
  {"vehicle-detection", 'v', 0, G_OPTION_ARG_INT, &vehicle_detection_enabled,
    "0: Disable vehicle detection, \
      1: Enable vehicle detection, \
      Default: vehicle detection enabled", NULL}
  ,
  {NULL}
  ,
};

static GstElement *pipeline = NULL, *tee_pre_decode = NULL;
static NvDsSRContext *nvdssrCtxInc = NULL;
static NvDsSRContext *nvdssrCtxStr = NULL;
static GMainLoop *loop = NULL;
FixedSizeCounter person_counter = FixedSizeCounter(ALARM_WINDOW);
FixedSizeCounter vehicle_counter = FixedSizeCounter(ALARM_WINDOW);

int 
createFolder(const char* folderPath) {
    if (mkdir(folderPath, 0755) == 0) {
        return 1; // Success
    } else {
        return 0; // Failed
    }
}

static gboolean
bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      LOG(INFO) << "[Deepstream] End of stream";
      g_main_loop_quit (loop);
      break;
    case GST_MESSAGE_ERROR:{
      gchar *debug;
      GError *error;
      gst_message_parse_error (msg, &error, &debug);
      LOG(ERROR) << "[Deepstream] ERROR from element " << GST_OBJECT_NAME (msg->src) << error->message;
      if (debug)
        LOG(ERROR) << "[Deepstream] Error details:  " << debug;
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


static gchar *
get_absolute_file_path (const gchar *cfg_file_path, gchar *file_path)
{
  gchar abs_cfg_path[PATH_MAX + 1];
  gchar *abs_file_path;
  gchar *delim;

  if (file_path && file_path[0] == '/') {
    return file_path;
  }

  if (!realpath (cfg_file_path, abs_cfg_path)) {
    g_free (file_path);
    return NULL;
  }

  // Return absolute path of config file if file_path is NULL.
  if (!file_path) {
    abs_file_path = g_strdup (abs_cfg_path);
    return abs_file_path;
  }

  delim = g_strrstr (abs_cfg_path, "/");
  *(delim + 1) = '\0';

  abs_file_path = g_strconcat (abs_cfg_path, file_path, NULL);
  g_free (file_path);

  return abs_file_path;
}


/* Function to adjust time and rename file */
void rename_stream_file(const std::string& input_file, const std::string& file_dirpath) {

    const int YEAR_OFFSET = 1900;
    const int MONTH_OFFSET = 1;

    // Find the position of the '-' character preceding the time
    size_t dash_pos = input_file.find('-');
    if (dash_pos == std::string::npos) {
        LOG(ERROR) << "[Deepstream] - [Stream Record] - Time not found in the input string \n";
        return;
    }
    std::string input_time = input_file.substr(dash_pos + 1, 6);
    
    // Find the position of the last underscore to get the date part
    size_t last_underscore_pos = input_file.rfind('_');
    if (last_underscore_pos == std::string::npos || last_underscore_pos <= dash_pos) {
        LOG(ERROR) << "[Deepstream] - [Stream Record] - Format of the file name is not as expected \n";
        return;
    }
    std::string date_str = input_file.substr(dash_pos - 8, 8);
    int year = std::stoi(date_str.substr(0, 4));
    int month = std::stoi(date_str.substr(4, 2));
    int day = std::stoi(date_str.substr(6, 2));
    
    struct tm file_tm = {};
    file_tm.tm_year = year - YEAR_OFFSET; // tm_year is years since 1900
    file_tm.tm_mon = month - MONTH_OFFSET;    // tm_mon is months since January (0-11)
    file_tm.tm_mday = day;
    file_tm.tm_hour = std::stoi(input_time.substr(0, 2));
    file_tm.tm_min = std::stoi(input_time.substr(2, 2));
    file_tm.tm_sec = std::stoi(input_time.substr(4, 2));
    file_tm.tm_isdst = -1;         // Automatically determine if DST is in effect
		
		// Convert the tm structure to time_t (UTC)
    time_t file_time_utc = timegm(&file_tm);

    struct tm *adjusted_tm = std::localtime(&file_time_utc);

    std::ostringstream oss;

    oss << std::put_time(adjusted_tm, "%Y%m%d-%H%M%S");
        
    oss << "_" << tzname[adjusted_tm->tm_isdst > 0 ? 1 : 0];

    std::string converted_time = oss.str();

     // Constructing full paths
    std::string original_path = file_dirpath + "/" + input_file;
    std::string new_file = file_dirpath + "/" + input_file.substr(0, dash_pos - 8) + converted_time + input_file.substr(input_file.rfind('.'));

    // Renaming the file
    if (rename(original_path.c_str(), new_file.c_str()) != 0) {
        LOG(ERROR) << "[Deepstream] - [Stream Record] - Error renaming the file \n";
    } else {
        LOG(INFO) << "[Deepstream] - [Stream Record] - File renamed successfully \n";
    }
}


static gboolean
set_tracker_properties (GstElement *nvtracker)
{
  gboolean ret = FALSE;
  GError *error = NULL;
  gchar **keys = NULL;
  gchar **key = NULL;
  GKeyFile *key_file = g_key_file_new ();
  g_object_set (G_OBJECT (nvtracker), "user-meta-pool-size", 320, NULL);
  if (!g_key_file_load_from_file (key_file, tracker_config_file, G_KEY_FILE_NONE,
          &error)) {
    LOG(ERROR) << "[Deepstream] - [Tracker] - Failed to load config file: " << error->message;
    return FALSE;
  }

  keys = g_key_file_get_keys (key_file, CONFIG_GROUP_TRACKER, NULL, &error);
  CHECK_ERROR (error);

  for (key = keys; *key; key++) {
    if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_WIDTH)) {
      gint width =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_WIDTH, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "tracker-width", width, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_HEIGHT)) {
      gint height =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GROUP_TRACKER_HEIGHT, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "tracker-height", height, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GPU_ID)) {
      guint gpu_id =
          g_key_file_get_integer (key_file, CONFIG_GROUP_TRACKER,
          CONFIG_GPU_ID, &error);
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "gpu_id", gpu_id, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_LL_CONFIG_FILE)) {
      char* ll_config_file = get_absolute_file_path (tracker_config_file,
                g_key_file_get_string (key_file,
                    CONFIG_GROUP_TRACKER,
                    CONFIG_GROUP_TRACKER_LL_CONFIG_FILE, &error));
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "ll-config-file", ll_config_file, NULL);
    } else if (!g_strcmp0 (*key, CONFIG_GROUP_TRACKER_LL_LIB_FILE)) {
      char* ll_lib_file = get_absolute_file_path (tracker_config_file,
                g_key_file_get_string (key_file,
                    CONFIG_GROUP_TRACKER,
                    CONFIG_GROUP_TRACKER_LL_LIB_FILE, &error));
      CHECK_ERROR (error);
      g_object_set (G_OBJECT (nvtracker), "ll-lib-file", ll_lib_file, NULL);
    } else {
      LOG(ERROR) << "[Deepstream] - [Tracker] - Unknown key " << *key << " for group " << CONFIG_GROUP_TRACKER;
    }
  }

  ret = TRUE;
done:
  if (error) {
    g_error_free (error);
  }
  if (keys) {
    g_strfreev (keys);
  }
  if (!ret) {
    g_printerr ("%s failed", __func__);
  }
  return ret;
}


static gpointer 
smart_record_callback(NvDsSRRecordingInfo *info, gpointer userData) {
    char *full_path = (char *)malloc(strlen(info->dirpath) + strlen(info->filename) + 2);
    guint64 incident_length = info->duration;     // in ms

    LOG(INFO) << "[Deepstream] - [SmartRecord] - Incident length is " << incident_length;

    if (!full_path) {
        LOG(ERROR) << "[Deepstream] - [SmartRecord] - Memory allocation error for file path";
        return NULL;
    }

    // Construct full_path
    snprintf(full_path, strlen(info->dirpath) + strlen(info->filename) + 2, "%s%s%s",
             info->dirpath,
             (info->dirpath[strlen(info->dirpath) - 1] != '/') ? "/" : "",
             info->filename);

    VLOG(1) << "[Deepstream] - [SmartRecord] - Video will be saved to: " << full_path;
    LOG(INFO) << "posting video on " << full_path;
    std::unordered_map<std::string, std::string> data;
    data["video_path"] = full_path;
    data["camera_id"] = mac;
    data["retries"] = "0";
    data["length"] = std::to_string(incident_length);
    nlohmann::json json_data = data;
    std::string message_body = json_data.dump();
    channel->BasicPublish(RABBITMQ_EXCHANGE_NAME, RABBITMQ_ROUTING_KEY, AmqpClient::BasicMessage::Create(message_body));
    return NULL;
}

static gpointer 
smart_record_callback_stream(NvDsSRRecordingInfo *info, gpointer userData) {
	std::string input_file = info->filename;
  std::string file_dirpath = info->dirpath;
	rename_stream_file(input_file, file_dirpath);
	return NULL;
}

void
smart_record_event_generator (gpointer data)
{
  NvDsSRSessionId sessId = 0;
  NvDsSRContext *ctx = (NvDsSRContext *) data;
  guint startTime = SMART_REC_START_TIME;
  guint duration = SMART_REC_DURATION;
  
  if (ctx->recordOn) {
    LOG(INFO) <<  "[Deepstream] - [SmartRecord] - Recording done for camera " << camera_id;
    if (NvDsSRStop (ctx, 0) != NVDSSR_STATUS_OK)
      LOG(ERROR) << "[Deepstream] - [SmartRecord] - Unable to stop recording for camera " << camera_id;
  } else {
    LOG(INFO) << "[Deepstream] - [SmartRecord] - Recording started for camera " << camera_id;
    if (NvDsSRStart (ctx, &sessId, startTime, duration,
            NULL) != NVDSSR_STATUS_OK)
      LOG(INFO) << "[Deepstream] - [SmartRecord] - Unable to start recording for camera " << camera_id;
  }
}

bool is_pgie_class_id_vehicle(int class_id){
  bool is_vehicle_class = false;
  for (int i = 0; i < PGIE_CLASS_IDS_SIZE; ++i) {
        if (class_id == vehicle_class_ids[i]) {
            is_vehicle_class = true;
            return true;
        }
    }

    return false;

}


void update_recording_interval() {
    if (recording_counter >= RECORDING_FREQUENCY_THRESHOLD) {
        // wait time increased if there has been many incidents 
        current_interval = std::min(current_interval + std::chrono::seconds(75), MAX_INTERVAL);
        recording_counter = 0; // reset when reaching maximum threshold in a certain interval 
    } else if (recording_counter > 0) {
        current_interval = std::max(current_interval - std::chrono::seconds(15), BASE_INTERVAL);
    }
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(current_interval).count();
    LOG(INFO) << seconds << "  " << recording_counter;
}


static GstPadProbeReturn
osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *) info->data;
    NvDsObjectMeta *obj_meta = NULL;
    int * velocity= NULL;
    NvDsMetaList * l_frame = NULL;
    NvDsMetaList * l_obj = NULL;
    NvDsDisplayMeta *display_meta = NULL;
    NvOFFlowVector *motion_data = NULL;
    int m_cols = 0, m_rows = 0;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);


    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL;
      l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
        /* Frame level decisions */
        for (NvDsMetaList * l_user = frame_meta->frame_user_meta_list;
                l_user != NULL; l_user = l_user->next) {
            NvDsUserMeta *user_meta = (NvDsUserMeta *) l_user->data;
            if (user_meta->base_meta.meta_type == NVDS_OPTICAL_FLOW_META){
                NvDsOpticalFlowMeta *meta = (NvDsOpticalFlowMeta *) user_meta->user_meta_data;
                m_rows = meta->rows;
                m_cols = meta->cols;
                motion_data = (NvOFFlowVector *) meta->data;
            }
        }
        int vehicles_moving = 0;
        int person_detected = 0;
        /* Object level decisions */
        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL;
                l_obj = l_obj->next) {
            obj_meta = (NvDsObjectMeta *) (l_obj->data);
            if (obj_meta->class_id == PGIE_CLASS_ID_PERSON && person_detection_enabled) {
                VLOG(2) << "[Deepstream] - [Alarm] - Person on Frame\n";
                guint keep_person = 1;
                // Check for ROI
                for (NvDsMetaList *l_user_meta = obj_meta->obj_user_meta_list; l_user_meta != NULL;
                    l_user_meta = l_user_meta->next) {
                    NvDsUserMeta *user_meta = (NvDsUserMeta *) (l_user_meta->data);
                    if(user_meta->base_meta.meta_type == NVDS_USER_OBJ_META_NVDSANALYTICS)
                    {
                        NvDsAnalyticsObjInfo * user_meta_data = (NvDsAnalyticsObjInfo *)user_meta->user_meta_data;
                        if (user_meta_data->roiStatus.size()){
                          // Person inside ROI, remove person
                          keep_person = 0;
                          VLOG(2) << "VLOG [Deepstream] - [Alarm] - Person was inside ROI, Removing\n";
                        }else{
                          // At least one person outside ROI, keep person & break
                          VLOG(2) << "VLOG [Deepstream] - [Alarm] - Person was outside ROI, adding\n";
                          keep_person = 1;
                          break;
                        }
                    }
                }
                person_detected += keep_person;
                
            }
            else if (is_pgie_class_id_vehicle(obj_meta->class_id) && vehicle_detection_enabled) {
                VLOG(2) << "[Deepstream] - [Alarm] - Vehicle on Frame\n";
                guint keep_vehicle = 1;
                // Check for ROI
                for (NvDsMetaList *l_user_meta = obj_meta->obj_user_meta_list; l_user_meta != NULL;
                    l_user_meta = l_user_meta->next) {
                    NvDsUserMeta *user_meta = (NvDsUserMeta *) (l_user_meta->data);
                    if(user_meta->base_meta.meta_type == NVDS_USER_OBJ_META_NVDSANALYTICS)
                    {
                        NvDsAnalyticsObjInfo * user_meta_data = (NvDsAnalyticsObjInfo *)user_meta->user_meta_data;
                        if (user_meta_data->roiStatus.size()){
                          // Vehicle inside ROI, remove vehicle
                          keep_vehicle = 0;
                          VLOG(2) << "VLOG [Deepstream] - [Alarm] - Vehicle was inside ROI, Removing\n";
                        }else{
                          // At least one vehicle outside ROI, keep vehicle & break
                          VLOG(2) << "VLOG [Deepstream] - [Alarm] - Vehicle was outside ROI, adding\n";
                          keep_vehicle = 1;
                          
                        }
                    }
                     
                }
                // check for vehicle motion if it is outside excluded zone
                if (keep_vehicle) {
                    VLOG(2) << "[Deepstream] - [Alarm] - Calculating vehicle movement\n";
                    NvOSD_RectParams &bbox = obj_meta->rect_params;
                    float x1 = bbox.left;
                    float y1 = bbox.top;
                    float x2 = bbox.left + bbox.width;
                    float y2 = bbox.top + bbox.height;
                    int block_size = 4;  // nvof blocks are 4x4 squares

                    int flow_row_start = y1 / block_size; 
                    int flow_col_start = x1 / block_size;
                    int flow_row_end = y2 / block_size;
                    int flow_col_end = x2 / block_size;

                    // Calculate total blocks within the bounding box
                    int total_blocks = (flow_row_end - flow_row_start + 1) * (flow_col_end - flow_col_start + 1);
                    // Movement detection
                    int blocks_with_movement = 0;
                    bool vehicle_moving = false; 
                    for (int row = flow_row_start; row <= flow_row_end; row++) {
                        for (int col = flow_col_start; col <= flow_col_end; col++) {
                            NvOFFlowVector *flow_vector = &motion_data[row * m_cols + col];
                            auto dx = flow_vector->flowx / 32.0f;
                            auto dy = flow_vector->flowy / 32.0f;
                            float magnitude = abs(dx) + abs(dy);
                            if (magnitude > BLOCK_MOTION_THRESHOLD) {
                                blocks_with_movement++;
                                // Check for defined threshold early to save iterations
                                if (blocks_with_movement >= total_blocks * BOX_MOTION_PERCENTAGE) {
                                    vehicle_moving = true;
                                    break; 
                                }
                            }
                        }
                        if (vehicle_moving) {
                            
                            VLOG(2) << "[Deepstream] - [Alarm] - Vehicle considered moving\n";
                            break;  
                        }
                    }
                    // Remove vehicles if no movement is there
                    if (!vehicle_moving) {
                        NvOSD_RectParams &rect_params = obj_meta->rect_params;
                        rect_params.border_color = (NvOSD_ColorParams) {0, 1, 0, 1};  // vehicle not moving then green {r,g,b,alp}
                        keep_vehicle = 0;
                    }
                }
                vehicles_moving += keep_vehicle;
            }
        }
        if (person_detected > 0) {
          person_counter.add(1);
        } else {
          person_counter.add(0);
        }
        if (vehicles_moving > 0){
          vehicle_counter.add(1);
        } else {
          vehicle_counter.add(0);
        }
    }
    
    // check whether processing is required for this frame.
    // if a recording is on we can skip
    // If the elapsed time is less than interval and we couldnt record more in the interval we can skip
    auto current_time = std::chrono::system_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::seconds> (current_time - last_alarm_generated);
    if (nvdssrCtxInc->recordOn || (elapsed_time < current_interval && recording_counter == RECORDING_FREQUENCY_THRESHOLD - 1)){
      person_counter.reset_counter();
      vehicle_counter.reset_counter();
      return GST_PAD_PROBE_OK;
    }

    // reset recording counters if there has been no incidents for interval limit
    if (elapsed_time > current_interval) {
      recording_counter = 0;
    }

    bool person = (person_counter.get_sum() > PERSON_DETECTED_FRAMES_LIMIT);
    bool vehicle = (vehicle_counter.get_sum() > VEHICLE_DETECTED_FRAMES_LIMIT);
    VLOG(4) << "[Deepstream] - [SmartRecord] - Person: " << person << "on camera " << camera_id;
    VLOG(4) << "[Deepstream] - [SmartRecord] - Vehicle: " << vehicle << "on camera " << camera_id;
    bool is_alarm = false;
    if (!nvdssrCtxInc->recordOn && (person || vehicle)){     
      is_alarm = true;
    }
    
    if (is_alarm) {
      smart_record_event_generator(nvdssrCtxInc);
      vehicle_counter.reset_counter();
      person_counter.reset_counter();
      recording_counter++;
      last_alarm_generated = std::chrono::system_clock::now();
      update_recording_interval();
    }

    return GST_PAD_PROBE_OK;
}


/* Funtion to record streams using smart record */
static GstPadProbeReturn
recordQue_sink_pad_buffer_probe (GstPad * pad,
                        GstPadProbeInfo * info,
                        gpointer u_data)
{
  NvDsSRSessionId sessId = 1;
  NvDsSRContext *ctx = (NvDsSRContext *) u_data;
  guint startTime = STREAM_REC_START_TIME;
  guint duration = STREAM_REC_DURATION;

  /* Check whether a recording session is still going on and recordbin's encorder is on reset to proceed */
  if (!nvdssrCtxStr->recordOn && nvdssrCtxStr->resetDone && !is_record_stopped){  
    LOG(INFO) << "[Deepstream] - [Stream Record] - Recording started for camera " << camera_id;
    if (NvDsSRStart (ctx, &sessId, startTime, duration,
            NULL) != NVDSSR_STATUS_OK){
      LOG(INFO) << "[Deepstream] - [Stream Record] - Unable to start recording for camera " << camera_id;
            }
  }  
  return GST_PAD_PROBE_OK;
}


/* copy function set by user. "data" holds a pointer to NvDsUserMeta*/
static double* copy_user_meta(gpointer data, gpointer user_data)
{
  NvDsUserMeta *user_meta = (NvDsUserMeta *)data;
  double *src_user_metadata = (double *) user_meta->user_meta_data;
  double *dst_user_metadata;

  memcpy(dst_user_metadata, src_user_metadata, sizeof(double));
  return (double *) dst_user_metadata;
}

/* release function set by user. "data" holds a pointer to NvDsUserMeta*/
static void release_user_meta(gpointer data, gpointer user_data)
{
    NvDsUserMeta *user_meta = (NvDsUserMeta *) data;
    if(user_meta->user_meta_data) {
        g_free(user_meta->user_meta_data);
        user_meta->user_meta_data = NULL;
    }
}


/* Signal handler function to finish recording stream and stop the pipeline if a Ctrl+C signal is received */
static void
signal_handler(int signum) {
    LOG(INFO) << "CTRL+C signal received! \n";
    ++ctrl_c_count;

    if (ctrl_c_count >= 3) {
      LOG(INFO) << "Ctrl+C pressed twice. Exiting immediately. \n";
      pid_t pid = getpid();
      int result = kill(pid, SIGTERM);
      if (!(result == 0)) {
          LOG(INFO) << "[Deepstream] - [SmartRecord] - Failed to send SIGTERM signal to terminate the current process. \n";
      }
    }
    
    if (nvdssrCtxStr->recordOn) {
      LOG(INFO) <<  "[Deepstream] - [SmartRecord] - Recording done for camera " << camera_id;
      if (NvDsSRStop (nvdssrCtxStr, 1) != NVDSSR_STATUS_OK){
        LOG(ERROR) << "[Deepstream] - [SmartRecord] - Unable to stop recording for camera " << camera_id;
      } else {
        LOG(INFO) <<  "[Deepstream] - [SmartRecord] - Recording stopped successfully for camera. \n" << camera_id;
        is_record_stopped = true;
      }
    }
   
    /* Wait until the encodebin of recordbin is in reset */
    if (nvdssrCtxStr->encodebin != NULL) {
      while (nvdssrCtxStr->resetDone == 0){
        LOG(INFO) << "[Deepstream] waiting till reset is done";
        }
    }

    LOG(INFO) << "[Deepstream] End of stream";
    g_main_loop_quit (loop);
}


static void
cb_newpad_audio_parsebin (GstElement * element, GstPad * element_src_pad, gpointer data)
{
  GstPad *sinkpad = gst_element_get_static_pad(nvdssrCtxInc->recordbin, "asink");
  if (gst_pad_link(element_src_pad, sinkpad) != GST_PAD_LINK_OK) {
    LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements not linked. Exiting.";
    g_main_loop_quit(loop);
  }
}

static void
cb_newpad (GstElement * element, GstPad * element_src_pad, gpointer data)
{

  GstCaps *caps = gst_pad_get_current_caps (element_src_pad);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);

  GstElement *depay_elem = (GstElement *) data;

  const gchar *media = gst_structure_get_string (str, "media");
  gboolean is_video = (!g_strcmp0 (media, "video"));
  gboolean is_audio = (!g_strcmp0 (media, "audio"));

  if (g_strrstr (name, "x-rtp") && is_video) {
    GstPad *sinkpad = gst_element_get_static_pad (depay_elem, "sink");
    if (gst_pad_link (element_src_pad, sinkpad) != GST_PAD_LINK_OK) {
      LOG(ERROR) << "[Deepstream] - [Pipeline] - Failed to link depay loader to rtsp src";
    }
    gst_object_unref (sinkpad);
    
    /* If the is_recording flag is 1, add a separate branch to record streams */
    if ((is_recording) && (sr_mode == 0 || sr_mode == 1)) {
        GstElement *parser_pre_str_recordbin;
        GstPad *recordQue_sink_pad;

        if (stream_enc == 0){
            parser_pre_str_recordbin =
              gst_element_factory_make ("h264parse", "parser_pre_str_recordbin");
        } else {
            parser_pre_str_recordbin =
              gst_element_factory_make ("h265parse", "parser_pre_str_recordbin");
        }
        
        gst_bin_add_many (GST_BIN (pipeline), parser_pre_str_recordbin, NULL);

        if (!gst_element_link_many (tee_pre_decode, parser_pre_str_recordbin,
              nvdssrCtxStr->recordbin, NULL)) {
          LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements not linked. Exiting.";
          g_main_loop_quit(loop);
        }
        gst_element_sync_state_with_parent(parser_pre_str_recordbin);

        /* Get the sinkpad of the recordbin's queue element  */
        recordQue_sink_pad = gst_element_get_static_pad (nvdssrCtxStr->recordQue, "sink");

        if (!recordQue_sink_pad){
          LOG(FATAL) << "[Deepstream] - [Pipeline] - recordQue_sink_pad failed. Exiting.\n";
        }
        else {
          /* Add a probe funtion to check whether any buffers passing through the sink pad */
          gst_pad_add_probe (recordQue_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                            recordQue_sink_pad_buffer_probe, nvdssrCtxStr, NULL);
        }
        /* Enable Ctrl+C signal handler and the pipeline stopping signal handler*/
        signal(SIGINT, signal_handler);
        signal(SIGUSR1, signal_handler);
    }
    

    if (!bbox_enabled && (sr_mode == 0 || sr_mode == 1)) {
      GstElement *parser_pre_inc_recordbin;
      if (stream_enc == 0){
        parser_pre_inc_recordbin =
            gst_element_factory_make ("h264parse", "parser_pre_inc_recordbin");
      } else {
        parser_pre_inc_recordbin =
            gst_element_factory_make ("h265parse", "parser_pre_inc_recordbin");
        }
      gst_bin_add_many (GST_BIN (pipeline), parser_pre_inc_recordbin, NULL);
      
      if (!gst_element_link_many (tee_pre_decode, parser_pre_inc_recordbin,
              nvdssrCtxInc->recordbin, NULL)) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements not linked. Exiting.";
        g_main_loop_quit(loop);
      }
      gst_element_sync_state_with_parent(parser_pre_inc_recordbin);
    }
  }

  if (g_strrstr (name, "x-rtp") && is_audio) {
    if (!bbox_enabled && (sr_mode == 0 || sr_mode == 2)) {
      GstElement *parser_pre_inc_recordbin =
          gst_element_factory_make ("parsebin", "audio-parser-pre-recordbin");

      gst_bin_add_many (GST_BIN (pipeline), parser_pre_inc_recordbin, NULL);

      GstPad *sinkpad = gst_element_get_static_pad(parser_pre_inc_recordbin, "sink");
      if (gst_pad_link(element_src_pad, sinkpad) != GST_PAD_LINK_OK) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements not linked. Exiting.";
        g_main_loop_quit(loop);
      }

      g_signal_connect(G_OBJECT(parser_pre_inc_recordbin), "pad-added", G_CALLBACK(cb_newpad_audio_parsebin), NULL);

      gst_element_sync_state_with_parent(parser_pre_inc_recordbin);
    }
  }

  gst_caps_unref (caps);
}

int
main (int argc, char *argv[])
{
    // Initialize Google’s logging library.
    google::InitGoogleLogging(argv[0]);

    google::SetLogDestination(google::ERROR, "/var/log/realtime/pipeline_error_logs_");

    LOG(INFO) << "[Deepstream] - Deepstream Logging Initialized";
    // Initialize RabbitMQ broker
    AmqpClient::Channel::OpenOpts opts;
    opts.host = RABBITMQ_HOST;
    opts.auth = AmqpClient::Channel::OpenOpts::BasicAuth{"guest", "guest"};
    channel = AmqpClient::Channel::Open(opts);
    channel->DeclareExchange(RABBITMQ_EXCHANGE_NAME, AmqpClient::Channel::EXCHANGE_TYPE_DIRECT);


  GstElement *streammux = NULL, *sink = NULL, *pgie = NULL, *source = NULL,
      *nvvidconv = NULL, *nvvidconv2 = NULL, *encoder_post_osd = NULL,
      *queue_pre_sink = NULL, *queue_post_osd = NULL, *parser_post_osd = NULL,
      *nvosd = NULL, *tee_post_osd = NULL, *queue_pre_decode = NULL,
      *depay_pre_decode = NULL, *decoder = NULL,  *nvvidconv3 = NULL,
      *swenc_caps = NULL, *nvtracker = NULL, *nvdsanalytics = NULL,
      *nvof = NULL, *stream_payloader = NULL, *stream_encoder = NULL,
      *stream_vidconv = NULL, *stream_queue=NULL;
  GstPad *osd_sink_pad = NULL, *pgie_src_pad = NULL;

  GstCaps *caps = NULL, *stream_caps = NULL;
  GstElement *cap_filter = NULL, *stream_caps_filter = NULL;

  GstBus *bus = NULL;
  guint bus_watch_id = 0;
  guint i = 0, num_sources = 1;

  guint pgie_batch_size = 0;

  int current_device = -1;
  cudaGetDevice(&current_device);
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  GOptionContext *gctx = NULL;
  GOptionGroup *group = NULL;
  GError *error = NULL;

  NvDsSRInitParams paramsInc = { 0 };
  NvDsSRInitParams paramsStr = { 0 };
  
  gctx = g_option_context_new ("RTSP Restreamer app");
  group = g_option_group_new ("rtsp_restreamer", NULL, NULL, NULL, NULL);
  g_option_group_add_entries (group, entries);

  g_option_context_set_main_group (gctx, group);
  g_option_context_add_group (gctx, gst_init_get_option_group ());

  GST_DEBUG_CATEGORY_INIT (NVDS_APP, "NVDS_APP", 0, NULL);

  if (!g_option_context_parse (gctx, &argc, &argv, &error)) {
    LOG(ERROR) << "[Deepstream] - " << error->message;
    LOG(ERROR) << "[Deepstream] - " << g_option_context_get_help (gctx, TRUE, NULL);
    return -1;
  }

  /* Check input arguments */
  if (argc < 2) {
    LOG(FATAL) << "[Deepstream] - Usage: " << argv[0] << "rtsp uri>";
    return -1;
  }

  if (argc > 2) {
    LOG(FATAL) << "[Deepstream] One rtsp_h264 uri supported Usage: " << argv[0] << "<rtsp uri>";
    return -1;
  }

  /* Standard GStreamer initialization */
  gst_init (&argc, &argv);
  loop = g_main_loop_new (NULL, FALSE);
  
  /* Config file paths */
  std::string tmp_folder = "tmp/";
  std::string cameraIDString = std::to_string(camera_id);
  std::string tracker_config_file_string = tmp_folder + cameraIDString + TRACKER_CONFIG_FILE;
  tracker_config_file = tracker_config_file_string.c_str();
  std::string pgie_config_file = tmp_folder + cameraIDString + PGIE_CONFIG_FILE;
  std::string nvanalytics_config_file = tmp_folder + cameraIDString + NVDSANALYTICS_CONFIG_FILE;

  /* Create gstreamer elements */
  /* Create Pipeline element that will form a connection of other elements */
  pipeline = gst_pipeline_new ("rtsp-restreamer-pipeline");

  source = gst_element_factory_make ("rtspsrc", "rtsp-source");
  g_object_set (G_OBJECT (source), "location", argv[1], NULL);
  // g_object_set (G_OBJECT (source), "protocols", PROTOCOL , NULL);

  if (stream_enc == 0){
    depay_pre_decode = gst_element_factory_make ("rtph264depay", "h264-depay");
  } else {
    depay_pre_decode = gst_element_factory_make ("rtph265depay", "h264-depay");
  }
  

  queue_pre_decode = gst_element_factory_make ("queue", "queue-pre-decode");

  if (!source || !depay_pre_decode || !queue_pre_decode) {
    LOG(FATAL) << "[Deepstream] - [Pipeline] - One element in source end could not be created.\n";
    return -1;
  }

  g_signal_connect (G_OBJECT (source), "pad-added",
      G_CALLBACK (cb_newpad), depay_pre_decode);
  /* Create tee which connects decoded source data and Smart record bin without bbox */
  tee_pre_decode = gst_element_factory_make ("tee", "tee-pre-decode");

  decoder = gst_element_factory_make ("nvv4l2decoder", "nvv4l2-decoder");

  streammux = gst_element_factory_make ("nvstreammux", "stream-muxer");

  /* Use nvinfer or nvinferserver to infer on batched frame. */
  if (pgie_type == 0) {
    pgie = gst_element_factory_make ("nvinfer", "primary-nvinference-engine");
  }
  else if (pgie_type == 1) {
    pgie = gst_element_factory_make ("nvinferserver", "primary-nvinference-engine");
  }

  /* tracker */
  nvtracker = gst_element_factory_make ("nvtracker", "tracker");

  nvdsanalytics = gst_element_factory_make ("nvdsanalytics", "analytics");
  
  if (running_mode == 2) {
      /* Use queue to connect to the sink after tee_post_osd element */
      queue_pre_sink = gst_element_factory_make ("queue", "queue-pre-sink");

      /* Use convertor to convert from NV12 to RGBA as required by nvosd */
      nvvidconv = gst_element_factory_make ("nvvideoconvert", "nvvideo-converter");

      /* Use convertor to convert from RGBA to CAPS filter data format */
      nvvidconv2 =
          gst_element_factory_make ("nvvideoconvert", "nvvideo-converter2");

      g_object_set (G_OBJECT (nvvidconv), "output-buffers", 5, NULL);
      g_object_set (G_OBJECT (nvvidconv2), "output-buffers", 5, NULL);

      /* Create OSD to draw on the converted RGBA buffer */
      nvosd = gst_element_factory_make ("nvdsosd", "nv-onscreendisplay");

      /* Create tee which connects to sink and Smart record bin with bbox */
      tee_post_osd = gst_element_factory_make ("tee", "tee-post-osd");

      caps = gst_caps_from_string ("video/x-raw(memory:NVMM), format=(string)I420");
      cap_filter =
          gst_element_factory_make ("capsfilter", "src_cap_filter_nvvidconv");
      g_object_set (G_OBJECT (cap_filter), "caps", caps, NULL);
      gst_caps_unref (caps);
  }

  /* create nv optical flow element */
  if (motion == 1) {
    nvof = gst_element_factory_make ("nvof", "nvopticalflow");
        if (!nvof) {
            LOG(FATAL) << "[Deepstream] - [Pipeline] - NVOF could not be created. Exiting.\n";
            return -1;
        }
        g_object_set (G_OBJECT (nvof), "preset-level", 0, NULL);
  }
  if (running_mode == 1) {
    sink = gst_element_factory_make ("fakesink", "nvvideo-renderer");    
  }
  else if (running_mode == 2) {
    stream_vidconv = gst_element_factory_make ("nvvideoconvert", "stream-vid-conv");
    stream_queue = gst_element_factory_make("queue", "streaming-queue");

    if (enc_type == 0){
      // Hardware encoders used
      LOG(INFO) << "[Deepstream] - [Pipeline] - Hardware encoders are used";
      stream_encoder = gst_element_factory_make ("nvv4l2h264enc", "stream-enc0");
      g_object_set (G_OBJECT (stream_encoder), "bitrate", 200, NULL);
      g_object_set (G_OBJECT (stream_encoder), "preset-level", 1, NULL);
      g_object_set (G_OBJECT (stream_encoder), "insert-sps-pps", 1, NULL);
    }
    else{
      // Software encoders used
      LOG(INFO) << "[Deepstream] - [Pipeline] - Software encoders are used";
      stream_encoder = gst_element_factory_make ("x264enc", "stream-enc1");
      g_object_set (G_OBJECT (stream_encoder), "bitrate", 200, NULL);
      g_object_set (G_OBJECT (stream_encoder), "speed-preset", 2, NULL);
    }

    stream_payloader = gst_element_factory_make ("rtph264pay", "stream-payloader");

    stream_caps = gst_caps_from_string ("application/x-rtp,encoding-name=H264,profile=constrained-baseline");
    stream_caps_filter = gst_element_factory_make ("capsfilter", "streaming_caps");
    g_object_set (G_OBJECT (stream_caps_filter), "caps", stream_caps, NULL);
    gst_caps_unref (stream_caps);

    sink = gst_element_factory_make("udpsink", "stream-sink");
    if (!stream_vidconv || !stream_queue || !stream_encoder || !stream_payloader || !stream_caps_filter || !sink) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - One element could not be created. Exiting.\n";
        return -1;
      }
    g_object_set(G_OBJECT(sink), "host", UDP_MULTICAST_IP, NULL);
    g_object_set(G_OBJECT(sink), "port", port, NULL);
    g_object_set(G_OBJECT(sink), "buffer-size", 100000, NULL);
    
  }

  if (running_mode == 1){
      if (!pgie || !nvtracker || !nvdsanalytics || !tee_pre_decode || !sink) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - One element could not be created. Exiting.\n";
        return -1;
      }
  }
  else if (running_mode == 2){
      if (!pgie || !nvtracker || !nvdsanalytics || !nvvidconv || !nvosd || !nvvidconv2 || !cap_filter
          || !tee_post_osd || !tee_pre_decode || !sink) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - One element could not be created. Exiting.\n";
        return -1;
      }
  }
  
  g_object_set (G_OBJECT (streammux), "live-source", 1, NULL);
  g_object_set (G_OBJECT (streammux), "buffer-pool-size", 5, NULL);
  g_object_set (G_OBJECT (nvdsanalytics), "config-file", nvanalytics_config_file.c_str(), NULL);

  g_object_set (G_OBJECT (streammux), "batch-size", num_sources, NULL);

  g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height",
      MUXER_OUTPUT_HEIGHT,
      "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);

  /* Configure the nvinfer element using the nvinfer config file. */
  g_object_set (G_OBJECT (pgie), "config-file-path", pgie_config_file.c_str(), NULL);
  

  /* Override the batch-size set in the config file with the number of sources. */
  g_object_get (G_OBJECT (pgie), "batch-size", &pgie_batch_size, NULL);
  if (pgie_batch_size != num_sources) {
    LOG(INFO) << 
        ("[Deepstream] - [Pipeline] - WARNING: Overriding infer-config batch-size (%d) with number of sources (%d)\n",
        pgie_batch_size, num_sources);
    g_object_set (G_OBJECT (pgie), "batch-size", num_sources, NULL);
  }

  /* Set the tracker config */
  if (!set_tracker_properties(nvtracker)) {
      LOG(FATAL) << "[Deepstream] - [Pipeline] - Failed to set tracker properties. Exiting.\n";
      return -1;
    }

  if (running_mode == 2){
    g_object_set (G_OBJECT (nvosd), "process-mode", OSD_PROCESS_MODE,
                  "display-text", OSD_DISPLAY_TEXT, NULL);
  }
  g_object_set (G_OBJECT (sink), "qos", 0, NULL);

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  /* Set up the pipeline
   * rtsp-source-> h264-depay -> tee-> queue -> decoder ->nvstreammux -> pgie -> nvvidconv -> nvosd -> nvvidconv -> caps_filter -> tee -> queue -> video-renderer
   *                                                                                                                                     |-> queue -> encoder -> parser -> recordbin
   */
  if (running_mode == 1 && motion == 1){
      gst_bin_add_many (GST_BIN (pipeline), source, depay_pre_decode,
          tee_pre_decode, queue_pre_decode, decoder, streammux, nvof, 
          pgie, nvtracker, nvdsanalytics, sink, NULL);
  }
  else if (running_mode == 1 && motion == 0){
      gst_bin_add_many (GST_BIN (pipeline), source, depay_pre_decode,
          tee_pre_decode, queue_pre_decode, decoder, streammux, 
          pgie, nvtracker, nvdsanalytics, sink, NULL);
  }
  else if (running_mode == 2 && motion == 1){
      gst_bin_add_many (GST_BIN (pipeline), source, depay_pre_decode,
          tee_pre_decode, queue_pre_decode, decoder, streammux, nvof,
          pgie, nvtracker, nvdsanalytics, nvvidconv,
          nvosd, nvvidconv2, cap_filter, tee_post_osd, queue_pre_sink,
          stream_vidconv, stream_queue, stream_encoder, 
          stream_payloader, stream_caps_filter, sink, NULL);
  }
  else if (running_mode == 2 && motion == 0){
      gst_bin_add_many (GST_BIN (pipeline), source, depay_pre_decode,
          tee_pre_decode, queue_pre_decode, decoder, streammux,
          pgie, nvtracker, nvdsanalytics, nvvidconv,
          nvosd, nvvidconv2, cap_filter, tee_post_osd, queue_pre_sink,
          stream_vidconv, stream_queue, stream_encoder, 
          stream_payloader, stream_caps_filter, sink, NULL);
  }

  /* Link the elements together till decoder */
  if (!gst_element_link_many (depay_pre_decode, tee_pre_decode,
          queue_pre_decode, decoder, NULL)) {
    LOG(FATAL) <<  "[Deepstream] - [Pipeline] - Elements could not be linked: 1. Exiting.\n";
    return -1;
  }

  /* Link decoder with streammux */
  GstPad *sinkpad, *srcpad;
  gchar pad_name_sink[16] = "sink_0";
  gchar pad_name_src[16] = "src";

  sinkpad = gst_element_get_request_pad (streammux, pad_name_sink);
  if (!sinkpad) {
    LOG(FATAL) << "[Deepstream] - [Pipeline] - Streammux request sink pad failed. Exiting.\n";
    return -1;
  }

  srcpad = gst_element_get_static_pad (decoder, pad_name_src);
  if (!srcpad) {
    LOG(FATAL) << "[Deepstream] - [Pipeline] - Decoder request src pad failed. Exiting.\n";
    return -1;
  }

  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK) {
    LOG(FATAL) << "[Deepstream] - [Pipeline] - Failed to link decoder to stream muxer. Exiting.\n";
    return -1;
  }

  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  /* Link the remaining elements of the pipeline to streammux */
  if (running_mode == 1 && motion == 1){
      if (!gst_element_link_many (streammux, nvof, pgie, nvtracker, nvdsanalytics,
               sink, NULL)) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements could not be linked on mode 1. Exiting.\n";
        return -1;
      }
  }
  else if (running_mode == 1 && motion == 0){
      if (!gst_element_link_many (streammux, pgie, nvtracker, nvdsanalytics,
               sink, NULL)) {
        LOG(FATAL) <<  "[Deepstream] - [Pipeline] - Elements could not be linked on mode 1. Exiting.\n";
        return -1;
      }
  }
  else if (running_mode == 2 && motion == 1){
      if (!gst_element_link_many (streammux, nvof, pgie, nvtracker, nvdsanalytics,
              nvvidconv, nvosd, nvvidconv2, cap_filter, tee_post_osd,
              queue_pre_sink, stream_vidconv, stream_queue,
              stream_encoder, stream_payloader, stream_caps_filter,
              sink, NULL)) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements could not be linked on mode 2. Exiting.\n";
        return -1;
      }
  }
  else if (running_mode == 2 && motion == 0){
      if (!gst_element_link_many (streammux, pgie, nvtracker, nvdsanalytics,
              nvvidconv, nvosd, nvvidconv2, cap_filter, tee_post_osd,
              queue_pre_sink, stream_vidconv, stream_queue,
              stream_encoder, stream_payloader, stream_caps_filter,
              sink, NULL)) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements could not be linked on mode 2. Exiting.\n";
        return -1;
      }
  }

  /* Parameters are set before creating record bin
   * User can set additional parameters e.g recorded file path etc.
   * Refer NvDsSRInitParams structure for additional parameters
   */
  char record_folder [20];
  sprintf(record_folder, "tmp/%d/videos/", camera_id);
  createFolder(record_folder);
  paramsInc.containerType = SMART_REC_CONTAINER;
  paramsInc.cacheSize = SMART_REC_CACHE_SIZE_SEC;
  paramsInc.defaultDuration = SMART_REC_DEFAULT_DURATION;
  paramsInc.callback = smart_record_callback;
  paramsInc.fileNamePrefix = file_name_prefix;
  paramsInc.dirpath = record_folder;

  if (NvDsSRCreate (&nvdssrCtxInc, &paramsInc) != NVDSSR_STATUS_OK) {
    LOG(FATAL) <<  "Failed to create smart record bin";
    return -1;
  }

  gst_bin_add_many (GST_BIN (pipeline), nvdssrCtxInc->recordbin, NULL);

  if (is_recording){
    /* Set parameters for the smart record stream record element*/
    std::time_t currentTime = std::time(nullptr);
    std::tm* localTime = std::localtime(&currentTime);

    char camera_record_folder [90];
    sprintf(camera_record_folder, "%s/%s", "recorded_streams", mac);
    createFolder(camera_record_folder);

    paramsStr.containerType = STREAM_REC_CONTAINER;
    paramsStr.cacheSize = STREAM_REC_CACHE_SIZE_SEC;
    paramsStr.defaultDuration = chunk_size;
    paramsStr.fileNamePrefix = stream_name_prefix;
    paramsStr.dirpath = camera_record_folder;
    paramsStr.width = STREAM_REC_WIDTH;
    paramsStr.height = STREAM_REC_HEIGHT;
    paramsStr.callback = smart_record_callback_stream;

    if (NvDsSRCreate (&nvdssrCtxStr, &paramsStr) != NVDSSR_STATUS_OK) {
      LOG(FATAL) <<  "Failed to create smart record bin";
      return -1;
    }

    gst_bin_add_many (GST_BIN (pipeline), nvdssrCtxStr->recordbin, NULL);
  }
  

  if (bbox_enabled == 1 && running_mode == 2) {
    /* Encode the data from tee before recording with bbox */
    if (enc_type == 0) {
        /* Hardware encoder used*/
        encoder_post_osd =
            gst_element_factory_make ("nvv4l2h264enc", "encoder-post-osd");

      } else if (enc_type == 1) {
        /* Software encoder used*/

        swenc_caps =  gst_element_factory_make ("capsfilter", NULL);

        GstCaps *enc_caps = NULL;

        enc_caps = gst_caps_from_string ("video/x-raw, width=640,height=360");

        g_object_set (G_OBJECT (swenc_caps), "caps", enc_caps, NULL);
        gst_caps_unref (enc_caps);

        encoder_post_osd =
            gst_element_factory_make ("x264enc", "encoder-post-osd");

        nvvidconv3 = gst_element_factory_make ("nvvideoconvert", "nvvidconv3");
        g_object_set (G_OBJECT (nvvidconv3), "output-buffers", 5, NULL);
        gst_bin_add_many (GST_BIN (pipeline), swenc_caps, nvvidconv3, NULL);
      }

    /* Parse the encoded data after osd component */
    parser_post_osd = gst_element_factory_make ("h264parse", "parser-post-osd");

    /* Use queue to connect the tee_post_osd to nvencoder */
    queue_post_osd = gst_element_factory_make ("queue", "queue-post-osd");

    gst_bin_add_many (GST_BIN (pipeline), queue_post_osd, encoder_post_osd,
        parser_post_osd, NULL);

    if (enc_type == 0) {
      if (!gst_element_link_many (tee_post_osd, queue_post_osd, encoder_post_osd,
              parser_post_osd, nvdssrCtxInc->recordbin, NULL)) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements not linked. Exiting. \n";
        return -1;
      }
    }
    else if (enc_type == 1) {
      /* Link swenc_caps and nvvidconv3 in case of software encoder*/
      if (!gst_element_link_many (tee_post_osd, nvvidconv3, queue_post_osd,
              encoder_post_osd, swenc_caps, parser_post_osd,
              nvdssrCtxInc->recordbin, NULL)) {
        LOG(FATAL) << "[Deepstream] - [Pipeline] - Elements not linked. Exiting. \n";
        return -1;
      }
    }
  }

  if (running_mode == 1){
      osd_sink_pad = gst_element_get_static_pad (nvdsanalytics, "sink");
  }
  else if (running_mode == 2){
      osd_sink_pad = gst_element_get_static_pad (nvosd, "sink");
  }
  if (!osd_sink_pad)
      LOG(FATAL) << "[Deepstream] - [Pipeline] - Unable to get sink pad\n";
  else
      gst_pad_add_probe (osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                         osd_sink_pad_buffer_probe, NULL, NULL);
      gst_object_unref (osd_sink_pad);
  
  gst_object_unref (pgie_src_pad);
  /* Set the pipeline to "playing" state */
  LOG(INFO) << "[Deepstream] - [Pipeline] - Now playing:";
  LOG(INFO) << (" %s", argv[i + 1]);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* Wait till pipeline encounters an error or EOS */
  LOG(INFO) << "[Deepstream] - [Pipeline] - Running...\n";
  g_main_loop_run (loop);

  if (pipeline && nvdssrCtxInc) {
    if(NvDsSRDestroy (nvdssrCtxInc) != NVDSSR_STATUS_OK)
    LOG(FATAL) << "[Deepstream] - [Pipeline] - Unable to destroy incident recording instance\n";
  }

  if (pipeline && nvdssrCtxStr) {
    if(NvDsSRDestroy (nvdssrCtxStr) != NVDSSR_STATUS_OK)
    LOG(FATAL) << "[Deepstream] - [Pipeline] - Unable to destroy stream recording instance\n";
  }
  
  /* Out of the main loop, clean up nicely */
  LOG(INFO) << ("[Deepstream] - [Pipeline] - Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  LOG(INFO) << ("[Deepstream] - [Pipeline] - Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
  return 0;
}
