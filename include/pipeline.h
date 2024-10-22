#include <gst/gst.h>
#include <iostream>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <cuda_runtime_api.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <stack>
#include <cmath>
#include <csignal>
#include "gst-nvdssr.h"
#include "gstnvdsmeta.h"
#include "nvds_analytics_meta.h"
#include "nvds_opticalflow_meta.h"
#include <ctime>
#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <unordered_map>
#include <chrono>

#include "FixedSizeCounter.h"

#pragma once

/* Config Files */
#define TRACKER_CONFIG_FILE "/configs/tracker_config.txt"
#define SMART_RECORD_LOG_FILE "/configs/smart_record.log"
#define PGIE_CONFIG_FILE "/configs/model_config.txt"
#define NVDSANALYTICS_CONFIG_FILE "/configs/config_nvdsanalytics.txt"

/* Alarm metrics */
#define ALARM_WINDOW 100
#define PERSON_DETECTED_FRAMES_LIMIT 20
#define VEHICLE_DETECTED_FRAMES_LIMIT 20
#define MOVEMENT_DETECTED_FRAMES_LIMIT 10
#define RECORD_WAIT_FRAMES_LIMIT 300
#define PGIE_CLASS_ID_PERSON 0
#define PGIE_CLASS_ID_VEHICLE {1,2,3,5,6,7}
#define IS_PERSON_DETECTION_ENABLED 1
#define IS_VEHICLE_DETECTION_ENABLED 1

// /* RTSP source*/
// #define PROTOCOL 4   //tcp protocol

/* Stream Muxer*/
#define MUXER_OUTPUT_WIDTH 640
#define MUXER_OUTPUT_HEIGHT 360
#define MUXER_BATCH_TIMEOUT_USEC 40000

/* OSD */
#define OSD_PROCESS_MODE 1
#define OSD_DISPLAY_TEXT 1
#define MAX_DISPLAY_LEN 64

/* Smart Recording */
#define SMART_REC_CONTAINER NVDSSR_CONTAINER_MP4
#define SMART_REC_CACHE_SIZE_SEC 15
#define SMART_REC_DEFAULT_DURATION 10
#define SMART_REC_START_TIME 2
#define SMART_REC_DURATION 8

/* Stream Recording */
#define STREAM_REC_CONTAINER NVDSSR_CONTAINER_MP4
#define STREAM_REC_CACHE_SIZE_SEC 15
#define STREAM_REC_DEFAULT_DURATION 10800
#define STREAM_REC_START_TIME 2
#define STREAM_REC_DURATION 0
#define STREAM_REC_HEIGHT 0
#define STREAM_REC_WIDTH 0

/* Tracker */
#define CONFIG_GROUP_TRACKER "tracker"
#define CONFIG_GROUP_TRACKER_WIDTH "tracker-width"
#define CONFIG_GROUP_TRACKER_HEIGHT "tracker-height"
#define CONFIG_GROUP_TRACKER_LL_CONFIG_FILE "ll-config-file"
#define CONFIG_GROUP_TRACKER_LL_LIB_FILE "ll-lib-file"
#define CONFIG_GPU_ID "gpu-id"
#define MAX_TRACKING_ID_LEN 16

/*UDP sink*/
#define UDP_MULTICAST_IP "127.0.0.1"

/*RabbitMQ publisher*/
#define RABBITMQ_HOST "localhost"
#define RABBITMQ_EXCHANGE_NAME "threat_detect"
#define RABBITMQ_ROUTING_KEY "threat_detect"  // queue name

/* Tracker config parsing */
#define CHECK_ERROR(error) \
    if (error) { \
        g_printerr ("Error while parsing config file: %s\n", error->message); \
        goto done; \
    }

/* Optical flow <> Movement*/
#define BLOCK_MOTION_THRESHOLD 0.20
#define BOX_MOTION_PERCENTAGE 0.4

/* Function definitions */
int  createFolder(const char*);

static gboolean
bus_call (GstBus *, GstMessage *, gpointer);

static gchar *
get_absolute_file_path (const gchar *, gchar *);

static gboolean
set_tracker_properties (GstElement *);

static gpointer
smart_record_callback (NvDsSRRecordingInfo *, gpointer);

void smart_record_event_generator (gpointer);

void reset_person_frame_counters();

static GstPadProbeReturn
osd_sink_pad_buffer_probe (GstPad *, GstPadProbeInfo *, gpointer);

static void
cb_newpad_audio_parsebin (GstElement *, GstPad *, gpointer);

static void
cb_newpad (GstElement *, GstPad *, gpointer);

