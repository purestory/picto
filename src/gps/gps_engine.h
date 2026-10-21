#pragma once

#include "framework.h"
#include <string>
#include <vector>

struct GpsPoint {
    std::wstring filePath;
    double latitude = 0.0;
    double longitude = 0.0;
};

// Master 3-Stage GPS Extraction Pipeline
bool ExtractGps(const std::wstring& filePath, double& outLat, double& outLon);

// Google Maps Navigation & Direct Browser Link
void OpenGoogleMap(double lat, double lon);
void OpenGoogleMapForPoints(const std::vector<GpsPoint>& points);
bool GenerateAndOpenLeafletMap(const std::vector<GpsPoint>& points);
