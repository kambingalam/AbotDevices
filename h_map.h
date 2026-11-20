#pragma once
// Tab .ino tidak boleh diawali preprocessor directive (bug ##line Arduino IDE).
#include <Arduino.h>
// ===========================================================================
//  MAP & HUD
// ===========================================================================
void startMapDisplay() {
  mapRequested = true;
  mapStartTime = millis();
  mapForNewRoute = false;
  if (routeReady) hudActive = false;
}

bool isImportantStep(const NavigationStep& step) {
  return step.important;
}

int nextImportantStep() {
  for (int i = 0; i < navStepCount; i++) {
    if (!isImportantStep(navSteps[i])) continue;
    if (navSteps[i].routeDistance < simulatedRouteDistance - HUD_PASSED_MARGIN_M) continue;
    return i;
  }
  return -1;
}

void drawMapVehicle(int x, int y) {
  gfxFillTriangle(x, y - 4, x - 3, y + 3, x + 3, y + 3);
  gfxPixel(x, y + 4);
}

void drawMapDestination(int x, int y) {
  gfxFrame(x - 2, y - 2, 5, 5);
  gfxPixel(x, y);
}

void drawMiniTurnIcon(const String& instruction, int cx, int cy) {
  String text = instruction;
  text.toUpperCase();
  if (text.indexOf("KIRI") >= 0 || text.indexOf("LEFT") >= 0) {
    gfxLine(cx + 5, cy, cx - 4, cy);
    gfxLine(cx - 4, cy, cx, cy - 4);
    gfxLine(cx - 4, cy, cx, cy + 4);
  } else if (text.indexOf("KANAN") >= 0 || text.indexOf("RIGHT") >= 0) {
    gfxLine(cx - 5, cy, cx + 4, cy);
    gfxLine(cx + 4, cy, cx, cy - 4);
    gfxLine(cx + 4, cy, cx, cy + 4);
  } else if (text.indexOf("PUTAR") >= 0 || text.indexOf("U-TURN") >= 0) {
    gfxCircle(cx, cy, 5);
    gfxLine(cx - 4, cy - 3, cx - 1, cy - 5, COLOR_BLACK);
    gfxPixel(cx, cy - 5, COLOR_BLACK);
  } else {
    gfxLine(cx, cy + 5, cx, cy - 5);
    gfxLine(cx, cy - 5, cx - 3, cy - 2);
    gfxLine(cx, cy - 5, cx + 3, cy - 2);
  }
}

void drawMapHeader() {
  gfxText(0, 0, "NAV");
  if (osrmTotalDistance > 0) {
    gfxText(38, 0, String(osrmTotalDistance / 1000.0, 1) + "k");
  }
  gfxText(91, 0, String(osrmTotalDuration / 60.0, 0) + "m");
  gfxLine(0, 9, 127, 9);
}

void drawMapFooter() {
  gfxLine(0, 52, 127, 52);
  int s = nextImportantStep();
  if (s >= 0 && s < navStepCount) {
    NavigationStep& step = navSteps[s];
    float distanceToTurn = step.routeDistance - simulatedRouteDistance;
    if (distanceToTurn < 0) distanceToTurn = 0;
    drawMiniTurnIcon(step.instruction, 7, 59);
    String label = (distanceToTurn >= 1000.0f)
                     ? String(distanceToTurn / 1000.0f, 1) + "k "
                     : String((int)distanceToTurn) + "m ";
    String instruction = step.instruction;
    if (instruction.length() > 11) instruction = instruction.substring(0, 11);
    gfxText(16, 55, label + instruction);
  } else {
    gfxText(16, 55, "RUTE AKTIF");
  }
}

void renderMap() {
  beginFrame();
  if (!routeReady || routePointCount < 2) {
    drawMapHeader();
    drawCenteredText("MENCARI RUTE", 28, 1);
    presentFrame();
    return;
  }
  const int centerX = 64;
  const int centerY = 42;
  const float pixelsPerMeter = 0.13f;
  const float behindMeters = 90.0f;
  const float aheadMeters = 280.0f;

  int seg = 0;
  while (seg < routePointCount - 2 && routeCumulativeDistance[seg + 1] < simulatedRouteDistance) seg++;
  float segStart = routeCumulativeDistance[seg];
  float segEnd = routeCumulativeDistance[seg + 1];
  float t = (segEnd > segStart)
              ? (simulatedRouteDistance - segStart) / (segEnd - segStart)
              : 0.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  double vehicleLat = routePoints[seg].lat + (routePoints[seg + 1].lat - routePoints[seg].lat) * t;
  double vehicleLon = routePoints[seg].lon + (routePoints[seg + 1].lon - routePoints[seg].lon) * t;
  double cosLat = cos(vehicleLat * DEG_TO_RAD);

  double hdx = (routePoints[seg + 1].lon - routePoints[seg].lon) * 111320.0 * cosLat;
  double hdy = (routePoints[seg + 1].lat - routePoints[seg].lat) * 110540.0;
  double hlen = sqrt(hdx * hdx + hdy * hdy);
  double sinB = 0.0, cosB = 1.0;
  if (hlen > 0.5) {
    sinB = hdx / hlen;
    cosB = hdy / hlen;
  }

  auto toScreen = [&](double lat, double lon, int& sx, int& sy) {
    double e = (lon - vehicleLon) * 111320.0 * cosLat;
    double n = (lat - vehicleLat) * 110540.0;
    double forward = e * sinB + n * cosB;
    double right = e * cosB - n * sinB;
    sx = centerX + (int)lround(right * pixelsPerMeter);
    sy = centerY - (int)lround(forward * pixelsPerMeter);
  };

  for (int i = 1; i < routePointCount; i++) {
    float d1 = routeCumulativeDistance[i - 1];
    float d2 = routeCumulativeDistance[i];
    if (d2 < simulatedRouteDistance - behindMeters) continue;
    if (d1 > simulatedRouteDistance + aheadMeters) break;
    int x1, y1, x2, y2;
    toScreen(routePoints[i - 1].lat, routePoints[i - 1].lon, x1, y1);
    toScreen(routePoints[i].lat, routePoints[i].lon, x2, y2);
    gfxLine(x1, y1, x2, y2);
  }

  {
    int dx, dy;
    toScreen(routePoints[routePointCount - 1].lat,
             routePoints[routePointCount - 1].lon, dx, dy);
    if (dx >= 3 && dx <= 124 && dy >= 13 && dy <= 49) drawMapDestination(dx, dy);
  }

  gfxFillRect(0, 0, SCREEN_WIDTH, 10, COLOR_BLACK);
  gfxFillRect(0, 52, SCREEN_WIDTH, SCREEN_HEIGHT - 52, COLOR_BLACK);

  drawMapVehicle(centerX, centerY);
  drawMapHeader();
  drawMapFooter();
  presentFrame();
}

void drawTurnArrow(const String& instruction) {
  String text = instruction;
  text.toUpperCase();
  int cx = 64, cy = 34;
  if (text.indexOf("KIRI") >= 0 || text.indexOf("LEFT") >= 0) {
    gfxLine(cx + 18, cy, cx - 12, cy);
    gfxLine(cx - 12, cy, cx - 2, cy - 8);
    gfxLine(cx - 12, cy, cx - 2, cy + 8);
  } else if (text.indexOf("KANAN") >= 0 || text.indexOf("RIGHT") >= 0) {
    gfxLine(cx - 18, cy, cx + 12, cy);
    gfxLine(cx + 12, cy, cx + 2, cy - 8);
    gfxLine(cx + 12, cy, cx + 2, cy + 8);
  } else if (text.indexOf("PUTAR") >= 0 || text.indexOf("U-TURN") >= 0) {
    gfxCircle(cx, cy, 14);
    gfxFillCircle(cx, cy - 14, 3, COLOR_BLACK);
  } else {
    gfxLine(cx, cy + 12, cx, cy - 12);
    gfxLine(cx, cy - 12, cx - 7, cy - 4);
    gfxLine(cx, cy - 12, cx + 7, cy - 4);
  }
}

void renderHUD() {
  beginFrame();
  int displayStep = (hudLockedStep >= 0 && hudLockedStep < navStepCount)
                      ? hudLockedStep
                      : nextImportantStep();
  if (displayStep >= 0 && displayStep < navStepCount) {
    NavigationStep& step = navSteps[displayStep];
    float distanceToTurn = step.routeDistance - simulatedRouteDistance;
    if (distanceToTurn < 0) distanceToTurn = 0;
    String distanceText = (distanceToTurn >= 1000.0f)
                            ? String(distanceToTurn / 1000.0f, 1) + "k"
                            : String((int)distanceToTurn) + "m";
    gfxText(0, 0, distanceText, 2);
    drawTurnArrow(step.instruction);
    String instruction = step.instruction;
    if (instruction.length() > 20) instruction = instruction.substring(0, 20);
    gfxText(0, 49, instruction);
    if (step.road.length() > 0) {
      String road = step.road;
      if (road.length() > 20) road = road.substring(0, 20);
      gfxText(0, 58, road);
    }
  } else {
    drawCenteredText("LANJUTKAN", 28, 1);
  }
  presentFrame();
}

// ===========================================================================
//  ARRIVAL
// ===========================================================================
void handleArrival() {
  if (arrivalActive) return;
  if (sleepModeActive) wakeFromSleep();
  arrivalActive = true;
  tripArrived = true;
  tripActive = false;
  arrivalStartedAt = millis();
  mapRequested = false;
  hudActive = false;
  hudLockedStep = -1;
  hudCompletedStep = navStepCount > 0 ? navStepCount - 1 : -1;
  companionActive = false;
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  conversationActive = false;
  infoModeActive = false;
  startupOnboardingActive = false;
  cancelThinking();
  setCharacterEmotion(CHARACTER_HAPPY);
  lastUserActivityMs = millis();
  linkSend("{\"t\":\"arrived\"}");
  abotLog("INFO", "TUJUAN TERCAPAI");
  sfxPlay(SFX_ARRIVE);
}

void renderArrival() {
  beginFrame();
  drawCenteredText("SUDAH SAMPAI!", 1, 1);
  gfxLine(25, 11, 102, 11);
  drawCenteredText("Pastikan barang", 16, 1);
  drawCenteredText("bawaan sudah lengkap", 26, 1);
  drawCenteredText("dan pembayaran sudah", 36, 1);
  drawCenteredText("dilakukan ya.", 46, 1);
  drawCenteredText("Terima kasih!", 56, 1);
  presentFrame();
  if (millis() - arrivalStartedAt >= ARRIVAL_DISPLAY_DURATION) {
    arrivalActive = false;
    tripArrived = true;
    tripActive = false;
    setCharacterEmotion(CHARACTER_DEFAULT);
    resetNaturalEyeBehavior();
    lastUserActivityMs = millis();
  }
}

void startNewTrip() {
  if (networkIsBusy()) return;
  markUserActivity();
  tripActive = true;
  tripArrived = false;
  arrivalActive = false;
  tripMapPending = false;
  routeReady = false;
  routeRequested = false;
  routeFromLink = false;
  lastLinkGpsAt = 0;
  lastOsrmAttempt = 0;
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
  systemState = STATE_READY;
  activeOsrmUrl = "";
  osrmFailCount = 0;
  osrmGaveUpAt = 0;
  destinationResolved = false;
  tripIntroTriggerAsk = true;
  linkRouteReceived = false;
  startTripIntroduction();
}

