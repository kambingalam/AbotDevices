#pragma once
// ===========================================================================
//  LINK HANDLERS
// ===========================================================================
void onLinkRoute(JsonDocument& doc) {
  JsonArray coords = doc["coords"].as<JsonArray>();
  size_t total = coords.size();

  if (total < 2) {
    linkSend("{\"t\":\"nack\",\"for\":\"route\",\"why\":\"no_coords\"}");
    return;
  }

  markUserActivity();
  Serial.printf("[LINK] >>> ROUTE dari HP (%u titik)\n", (unsigned)total);

  osrmTotalDistance = doc["dist"] | 0.0f;
  osrmTotalDuration = doc["dur"] | 0.0f;

  routeStride = (int)((total + MAX_ROUTE_POINTS - 1) / MAX_ROUTE_POINTS);
  if (routeStride < 1) routeStride = 1;

  routePointCount = 0;
  size_t i = 0;
  for (JsonArray c : coords) {
    bool isLast = (i == total - 1);
    if ((i % routeStride) == 0 || isLast) {
      int slot = (routePointCount < MAX_ROUTE_POINTS)
                   ? routePointCount++
                   : (MAX_ROUTE_POINTS - 1);
      routePoints[slot].lon = c[0].as<double>();
      routePoints[slot].lat = c[1].as<double>();
    }
    i++;
  }

  int validCount = 0;
  for (int k = 0; k < routePointCount; k++) {
    if (routePoints[k].lat < -90.0 || routePoints[k].lat > 90.0 || routePoints[k].lon < -180.0 || routePoints[k].lon > 180.0) continue;
    if (validCount != k) routePoints[validCount] = routePoints[k];
    validCount++;
  }
  routePointCount = validCount;

  if (routePointCount < 2) {
    linkSend("{\"t\":\"nack\",\"for\":\"route\",\"why\":\"no_valid_coords\"}");
    return;
  }

  routeGeometryDistance = 0;
  routeCumulativeDistance[0] = 0;
  for (int k = 1; k < routePointCount; k++) {
    double lat1 = routePoints[k - 1].lat;
    double lon1 = routePoints[k - 1].lon;
    double lat2 = routePoints[k].lat;
    double lon2 = routePoints[k].lon;
    double dx = (lon2 - lon1) * 111320.0 * cos(lat1 * DEG_TO_RAD);
    double dy = (lat2 - lat1) * 110540.0;
    routeGeometryDistance += sqrt(dx * dx + dy * dy);
    routeCumulativeDistance[k] = routeGeometryDistance;
  }

  if (routeGeometryDistance < 50.0f) {
    linkSend("{\"t\":\"nack\",\"for\":\"route\",\"why\":\"degenerate_route\"}");
    routeReady = false;
    return;
  }

  linkDistScale = 1.0f;
  if (osrmTotalDistance > 100.0f && routeGeometryDistance > 100.0f) {
    linkDistScale = routeGeometryDistance / osrmTotalDistance;
    if (linkDistScale < 0.5f || linkDistScale > 1.5f) linkDistScale = 1.0f;
  }

  navStepCount = 0;
  for (JsonObject s : doc["steps"].as<JsonArray>()) {
    if (navStepCount >= MAX_NAV_STEPS) break;
    NavigationStep& nav = navSteps[navStepCount];
    nav.instruction = s["instr"] | "LANJUT";
    nav.road = s["road"] | "";
    nav.distance = s["dist"] | 0.0f;
    int idx = (s["idx"] | 0) / routeStride;
    if (idx >= routePointCount) idx = routePointCount - 1;
    if (idx < 0) idx = 0;
    nav.routeIndex = idx;
    nav.routeDistance = (s["routeDist"] | 0.0f) * linkDistScale;
    nav.important = s["imp"] | false;
    navStepCount++;
  }

  for (int a = 1; a < navStepCount; a++) {
    NavigationStep key = navSteps[a];
    int b = a - 1;
    while (b >= 0 && navSteps[b].routeDistance > key.routeDistance) {
      navSteps[b + 1] = navSteps[b];
      b--;
    }
    navSteps[b + 1] = key;
  }

  routeReady = true;
  routeRequested = false;
  routeFromLink = true;
  linkRouteReceived = true;
  activeOsrmUrl = "";
  osrmFailCount = 0;
  tripIntroTriggerAsk = false;
  tripActive = true;
  tripArrived = false;
  arrivalActive = false;
  initialMapShown = true;
  destinationAskActive = false;

  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  hudActive = false;
  hudShownAt = 0;
  navigationAIStarted = false;
  systemState = STATE_READY;

  speedMps = 8.0f;
  speedLastAt = 0;
  speedLastDist = 0;
  drLastMs = 0;
  lastLinkGpsAt = 0;

  skipStartupSequence();
  tripIntroActive = false;
  tripMapPending = false;

  startMapDisplay();
  mapForNewRoute = true;
  pendingNavigationAI = NAV_AI_ON_ROUTE;

  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"t\":\"ack\",\"for\":\"route\",\"pts\":%d,\"steps\":%d,\"scale\":%.3f}",
           routePointCount, navStepCount, linkDistScale);
  linkSend(String(buf));
}

float snapToRoute(double lat, double lon, int from, int to, float& along) {
  double cosLat = cos(lat * DEG_TO_RAD);
  float best = 1e9f;
  along = simulatedRouteDistance;
  if (from < 0) from = 0;
  if (to > routePointCount - 2) to = routePointCount - 2;

  for (int i = from; i <= to; i++) {
    double ax = (routePoints[i].lon - lon) * 111320.0 * cosLat;
    double ay = (routePoints[i].lat - lat) * 110540.0;
    double bx = (routePoints[i + 1].lon - lon) * 111320.0 * cosLat;
    double by = (routePoints[i + 1].lat - lat) * 110540.0;
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    double t = (len2 > 0.0) ? -(ax * dx + ay * dy) / len2 : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double px = ax + t * dx, py = ay + t * dy;
    float d = (float)sqrt(px * px + py * py);
    if (d < best) {
      best = d;
      along = routeCumulativeDistance[i] + (float)t * (routeCumulativeDistance[i + 1] - routeCumulativeDistance[i]);
    }
  }
  return best;
}

void onLinkGps(JsonDocument& doc) {
  if (!routeReady || routePointCount < 2) return;
  float dist = simulatedRouteDistance;

  if (!doc["progress"].isNull()) {
    float rawP = doc["progress"].as<float>();
    if (isnan(rawP) || isinf(rawP)) return;
    float p = rawP * linkDistScale;
    if (p < simulatedRouteDistance - 40.0f) return;
    if (p < 0.0f) return;
    dist = p;
  } else if (!doc["lat"].isNull() && !doc["lon"].isNull()) {
    double lat = doc["lat"].as<double>();
    double lon = doc["lon"].as<double>();
    if (isnan(lat) || isinf(lat) || isnan(lon) || isinf(lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;
    float along = 0;
    int from = simulatedRouteIndex - 2;
    int to = simulatedRouteIndex + 30;
    float off = snapToRoute(lat, lon, from, to, along);
    if (off > OFFROUTE_METERS) off = snapToRoute(lat, lon, 0, routePointCount - 2, along);
    if (off > OFFROUTE_METERS) {
      if (millis() - lastOffRouteReportAt > OFFROUTE_REPORT_MS) {
        lastOffRouteReportAt = millis();
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"offroute\",\"lat\":%.6f,\"lon\":%.6f,\"off\":%.0f}",
                 lat, lon, off);
        linkSend(String(buf));
        triggerCharacterReaction(CHARACTER_CONFUSED);
      }
      return;
    }
    if (along < simulatedRouteDistance - 40.0f) return;
    dist = along;
  } else {
    return;
  }

  lastLinkGpsAt = millis();
  drLastMs = millis();
  if (dist < 0) dist = 0;
  if (dist > routeGeometryDistance) dist = routeGeometryDistance;
  updateSpeedEstimate(dist);
  simulatedRouteDistance = dist;
  syncRouteProgress();
  if (simulatedRouteDistance >= routeGeometryDistance - 15.0f) {
    if (tripActive && !tripArrived && !arrivalActive) handleArrival();
  }
}

void onLinkTripStart() {
  markUserActivity();
  skipStartupSequence();
  tripActive = true;
  tripArrived = false;
  arrivalActive = false;
  tripMapPending = false;
  routeReady = false;
  routeRequested = false;
  routePointCount = 0;
  navStepCount = 0;
  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  hudActive = false;
  hudShownAt = 0;
  mapRequested = false;
  navigationAIStarted = false;
  pendingNavigationAI = false;
  activeOsrmUrl = "";
  osrmFailCount = 0;
  linkRouteReceived = false;
  routeFromLink = false;
  tripIntroTriggerAsk = false;
  lastLinkGpsAt = 0;
  speedMps = 8.0f;
  speedLastAt = 0;
  drLastMs = 0;
  destinationAskActive = false;
  tripIntroActive = true;
  tripIntroStartedAt = millis();
  setCharacterEmotion(CHARACTER_DEFAULT);
  resetNaturalEyeBehavior();
}

void onLinkTripEnd() {
  tripActive = false;
  tripArrived = false;
  arrivalActive = false;
  tripMapPending = false;
  routeReady = false;
  routeRequested = false;
  routeFromLink = false;
  linkRouteReceived = false;
  mapRequested = false;
  routePointCount = 0;
  navStepCount = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  lastLinkGpsAt = 0;
  pendingNavigationAI = false;
  navigationAIStarted = false;
  activeOsrmUrl = "";
  hudActive = false;
  initialMapShown = false;
  setCharacterEmotion(CHARACTER_DEFAULT);
  resetNaturalEyeBehavior();
  linkSend("{\"t\":\"trip_ended\",\"route\":0,\"trip\":0}");
}

void onLinkReset() {
  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  hudActive = false;
  hudShownAt = 0;
  arrivalActive = false;
  tripArrived = false;
  tripActive = true;
  speedLastAt = 0;
  drLastMs = 0;
}

void onLinkPing() {
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"t\":\"pong\",\"ver\":\"" FW_VERSION "\",\"up\":%lu}",
           millis() / 1000UL);
  linkSend(String(buf));
}

// ===========================================================================
//  ROUTING UTILITIES
// ===========================================================================
int decodePolyline(const String& encoded, RoutePoint* out, int maxPoints) {
  int idx = 0, count = 0, lat = 0, lng = 0;
  int len = encoded.length();
  while (idx < len && count < maxPoints) {
    int b, shift, result;
    shift = 0;
    result = 0;
    do {
      if (idx >= len) return count;
      b = encoded.charAt(idx++) - 63;
      result |= (b & 0x1f) << shift;
      shift += 5;
    } while (b >= 0x20);
    int dlat = ((result & 1) ? ~(result >> 1) : (result >> 1));
    lat += dlat;
    shift = 0;
    result = 0;
    do {
      if (idx >= len) return count;
      b = encoded.charAt(idx++) - 63;
      result |= (b & 0x1f) << shift;
      shift += 5;
    } while (b >= 0x20);
    int dlng = ((result & 1) ? ~(result >> 1) : (result >> 1));
    lng += dlng;
    out[count].lat = lat / 1e5;
    out[count].lon = lng / 1e5;
    count++;
  }
  return count;
}

String buildOsrmUrl(float fromLat, float fromLon, float toLat, float toLon) {
  String url = "http://router.project-osrm.org/route/v1/driving/";
  url += String(fromLon, 6) + "," + String(fromLat, 6) + ";";
  url += String(toLon, 6) + "," + String(toLat, 6);
  url += "?overview=simplified&geometries=polyline&steps=true";
  return url;
}

