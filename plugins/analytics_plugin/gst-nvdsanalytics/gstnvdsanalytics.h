/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __GST_NVDSANALYTICS_H__
#define __GST_NVDSANALYTICS_H__

#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include "nvbufsurface.h"
#include "gst-nvquery.h"
#include "gstnvdsmeta.h"
#include "nvds_analytics.h"
#include "nvds_analytics_meta.h"
#include "process_source.h"

/* Package and library details required for plugin_init */
#define PACKAGE "nvdsanalytics"
#define VERSION "1.0"
#define LICENSE "Proprietary"
#define DESCRIPTION "NVIDIA dsanalytics plugin for integration with DeepStream on DGPU/Jetson"
#define BINARY_PACKAGE "NVIDIA DeepStream dsanalytics plugin"
#define URL "http://nvidia.com/"


G_BEGIN_DECLS
/* Standard boilerplate stuff */
typedef struct _GstNvDsAnalytics GstNvDsAnalytics;
typedef struct _GstNvDsAnalyticsClass GstNvDsAnalyticsClass;

/* Standard boilerplate stuff */
#define GST_TYPE_DSANALYTICS (gst_nvdsanalytics_get_type())
#define GST_NVDSANALYTICS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_DSANALYTICS,GstNvDsAnalytics))
#define GST_NVDSANALYTICS_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_DSANALYTICS,GstNvDsAnalyticsClass))
#define GST_NVDSANALYTICS_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_DSANALYTICS, GstNvDsAnalyticsClass))
#define GST_IS_NVDSANALYTICS(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_DSANALYTICS))
#define GST_IS_NVDSANALYTICS_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_DSANALYTICS))
#define GST_NVDSANALYTICS_CAST(obj)  ((GstNvDsAnalytics *)(obj))

struct _GstNvDsAnalytics
{
  GstBaseTransform base_trans;

  // Context of the custom algorithm library
  //DsExampleCtx *nvdsanalyticslib_ctx;

  // Unique ID of the element. The labels generated by the element will be
  // updated at index `unique_id` of attr_info array in NvDsObjectParams.
  guint unique_id;

  // Frame number of the current input buffer
  guint64 batch_num;

  // Input video info (resolution, color format, framerate, etc)
  GstVideoInfo video_info;

  // Resolution at which analytics algorithms were configured
  gint configuration_width;
  gint configuration_height;

  // Amount of objects processed in single call to algorithm
  guint batch_size;

  // Config file path for dsanalytics
  gchar *config_file_path;

  // Config file parsing status for dsanalytics
  gboolean config_file_parse_successful;

  std::unordered_map<gint, StreamInfo> *stream_analytics_info;

  std::unordered_map<gint, NvDsAnalyticCtxUptr> *stream_analytics_ctx;

  GMutex analytic_mutex;

  gboolean enable;

  //Size of osd font
  guint font_size;

  //Type of osd modes, full /partial/completely off
  guint osd_mode;

  //Window in ms for obj count
  guint obj_cnt_win_in_ms;

  gboolean display_obj_cnt;
};

// Boiler plate stuff
struct _GstNvDsAnalyticsClass
{
  GstBaseTransformClass parent_class;
};

GType gst_nvdsanalytics_get_type (void);

G_END_DECLS
#endif /* __GST_NVDSANALYTICS_H__ */
