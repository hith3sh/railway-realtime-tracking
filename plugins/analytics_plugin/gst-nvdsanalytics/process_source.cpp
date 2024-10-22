#include "process_source.h"

double intersectionArea(const cv::Mat& mat, const cv::Rect& rect) {
    // draw rectangle in white to represent bounding box
    cv::Mat rectMask = cv::Mat::zeros(mat.size(), CV_8UC1);
    cv::rectangle(rectMask, rect, cv::Scalar(255), cv::FILLED);

    // Find intersection between the zone and rectangle
    cv::Mat intersection;
    cv::bitwise_and(mat, rectMask, intersection);

    // Calculate the area of intersection
    int nonzeroPixels = cv::countNonZero(intersection);
    // Assuming binary mask
    double area = nonzeroPixels;

    return area;
}

void processSource(NvDsAnalyticProcessParams &process_params, StreamInfo &stream_info)
{
    std::vector <ROIInfo> roi_vector = stream_info.roi_info;
    
    int frame_height = stream_info.config_height;
    int frame_width = stream_info.config_width;
    int roi_counter = 1;
    
    // Initialize the grayscale mat in black
    cv::Mat mat = cv::Mat::zeros(frame_height, frame_width, CV_8UC1);

    // Iterate through each ROI and fill them inside the mat in white
    for (const auto& roi_instance : roi_vector) {
        
        std::vector<cv::Point> pts;
        for (const auto& pair : roi_instance.roi_pts) {
            pts.emplace_back(pair.first, pair.second);
        }
        std::vector<std::vector<cv::Point>> roi_zone = {pts};

        cv::fillPoly(mat, roi_zone, cv::Scalar(255));

        roi_counter++;
    }

    // Iterate through each detected object
    int obj_cnt = 0;
    for (auto& obj : process_params.objList) {
        // Assuming bbox points are (left, top) and (left + width, top + height)
        int bbox_center_x = obj.left + obj.width / 2;
        int bbox_center_y = obj.top + obj.height / 2;
        cv::Rect rect(bbox_center_x - obj.width / 2, bbox_center_y - obj.height / 2, obj.width, obj.height);

        double rectArea = obj.width * obj.height;
        // To avoid division by very small or zero values
        rectArea += 1e-5;

        double intersection = intersectionArea(mat, rect);

        double inter_rectangle_ratio  = intersection / rectArea;
        
        bool inside_roi = false;
        if (inter_rectangle_ratio >= EXCLUDED_ZONE_PERCENTAGE){
            inside_roi = true;
        }
        
        if (inside_roi) {
            // ensures uniqueness by appending a counter value to roi_label
            // Here roi_label = "RF"
            std::string roi_label = "RF" + std::to_string(roi_counter);
            // Update objInROIcnt for the current ROI
            process_params.objInROIcnt[roi_label]++;
            // Attach metadata to the object
            obj.roiStatus.push_back("RF");
            process_params.objList[obj_cnt].str_obj_status = "in";
        }
        // Update object counts for each status
        process_params.objCnt[obj.class_id]++;        
        obj_cnt++;
    }
}