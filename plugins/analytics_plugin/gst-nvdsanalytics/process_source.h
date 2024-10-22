#ifndef PROCESS_SOURCE_H
#define PROCESS_SOURCE_H

#include <opencv2/opencv.hpp>
#include <vector>
#include "nvds_analytics.h"
#include <string>

#define EXCLUDED_ZONE_PERCENTAGE 0.8

void processSource(NvDsAnalyticProcessParams &process_params, StreamInfo &stream_info);

#endif // PROCESS_SOURCE_H