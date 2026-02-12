#pragma once
// ============================================================================
// RKNN-based object detector
//
// Wraps the RKNN runtime API for YOLOv8 inference on the RK3566 NPU.
// The detector accepts raw RGB frames and returns bounding box detections.
//
// This module is a clean C++ wrapper around the RKNN C API.  The post-
// processing (anchor decoding, NMS) is adapted from the existing
// YoloV8-NPU/src/postprocess.cpp.
// ============================================================================

#include "soulcam.h"
#include <vector>

namespace sc {

// Opaque detector handle
struct Detector;

// Create a detector from an RKNN model file.
// Returns nullptr on failure.
Detector* detector_create(const RknnConfig& cfg);

// Destroy the detector and release NPU resources.
void detector_destroy(Detector* det);

// Run inference on an RGB frame.
// - data:     pointer to RGB pixel data (row-major, 3 bytes per pixel)
// - size:     total byte count (width * height * channels)
// - width, height, channels: frame dimensions (channels must be 3)
// - out:      detection results are appended here
// Returns 0 on success.
int detector_infer(Detector* det,
                   const uint8_t* data, size_t size,
                   int width, int height, int channels,
                   std::vector<Detection>& out);

// Get model input dimensions.
void detector_get_input_size(const Detector* det,
                              int& width, int& height, int& channels);

}  // namespace sc
