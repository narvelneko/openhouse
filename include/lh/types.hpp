// Core data types shared by transports and the tracking library.
//
// These are the *normalized* events that cross the transport/tracking boundary.
// They mirror the callbacks CLighthouseOpticalThread / CLighthouseIMUThread deliver
// to CSwSyncDetectSource in driver_lighthouse.so (see ai/driver/interfaces.md), so any
// transport (USB, network, replay) that can produce them can drive the tracker.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lh {

// Device time in seconds (device 48 MHz tick counter, unwrapped to 64 bits and
// scaled by 1/48e6). Each device has its own time base.
using DeviceTime = double;

// Stable per-device identifier chosen by the transport (e.g. "LHR-0704E07A").
using DeviceId = std::string;

constexpr double kTicksPerSecond = 48e6;

struct ImuSample {
  DeviceTime time = 0;
  std::array<double, 3> accel{};  // m/s^2, raw (uncalibrated) device frame
  std::array<double, 3> gyro{};   // rad/s, raw (uncalibrated) device frame
  uint32_t saturated = 0;         // bit i: component i (ax,ay,az,gx,gy,gz) clipped
};

// LH1-style light pulse: one photodiode envelope (report 0x21/0x25, or Watchman).
struct LightPulse {
  DeviceTime time = 0;   // start of pulse
  double width = 0;      // seconds; < 0 if unknown
  int sensor = 0;        // 0..31
};

// LH2 demodulated beam word, report 0x28 word with bit1 == 0.
// Delivered to IDisambiguator slot 9 in the original.
struct Lh2BeamWord {
  DeviceTime time = 0;
  int channel = 0;  // LH2 channel set by preceding control byte
  int bit26 = 0;
  int bit27 = 0;
};

// LH2 sensor word, report 0x28 word with bit1 == 1.
// Delivered to IDisambiguator slot 10 in the original.
struct Lh2SensorWord {
  DeviceTime time = 0;
  int channel = 0;
  int value = 0;  // bits 27..31 of the word
};

enum class DeviceEventType : int {
  PulseWidthUnknown = 3,      // width field 0xffff
  ImuReportComplete = 0xb,
  Lh2ChannelConflict = 0xe,   // "two bases share light channel %d"
  SyncPulseSeen = 0xf,        // pulse with LH1 sync-like width
};

struct DeviceEvent {
  DeviceTime time = 0;
  int type = 0;  // DeviceEventType or other raw value
  int arg = 0;
};

// Auxiliary sensor events (sensor ids >= 0xf7 on LH1 reports, 0x28 control 3).
struct AuxEvent {
  DeviceTime time = 0;
  int id = 0;
  int value = 0;
};


// Controller input as framed by the wireless protocol. Only the framing is decoded
// here; what the bits mean depends on the device (see runtime/index_input.hpp).
enum class InputFormat : int {
  Legacy = 0,   // controller sub-protocol 0: buttons / trigger / 2 axes / 8-byte extended
  Raw = 1,      // controller sub-protocol 1: undecoded bytes in `raw`
  Compact = 2,  // controller sub-protocol 2: buttons, trigger, extra, int8 axes
};

struct InputReport {
  DeviceTime time = 0;
  InputFormat format = InputFormat::Legacy;
  bool hasButtons = false, hasTrigger = false, hasAxes = false, hasExtended = false;
  uint8_t buttons = 0, trigger = 0, extra = 0;
  int16_t axisX = 0, axisY = 0;          // first/second 16-bit word of the axis block
  std::array<uint8_t, 8> extended{};     // libsurvive "gen 2" record (type byte first)
  std::vector<uint8_t> raw;              // Raw format only
};

// Battery status byte: bits 0..6 percent, bit 7 charging (libsurvive handle_battery).
struct BatteryReport {
  DeviceTime time = 0;
  uint8_t raw = 0;
};

// A Doorman Dongle's repeater role and status (dongle firmware 0x0031+, see ai/doorman/protocol.md), read by
// the driver each second. Rates are per second since the previous read; there are none right after a dongle
// restart or role change.
struct DoormanRepeaterStatus {
  std::string serial;       // the dongle's serial ("OHDG-...")
  int board = -1;           // 0 Pro Micro, 1 Holyiot, 2 PCA10059, 3 Kuroshin (info byte 57), -1 not known
  int role = -1;            // -1: firmware without repeaters (or not read yet), 0 main, 1 repeater
  bool autoRole = false;    // a repeater on its own (it heard a main with devices): still a dongle to pair to
  uint32_t netBase = 0;     // its own network base
  // main: the repeater ids it uses now (one bit each), and the status of each
  int mask = 0;
  struct Repeater {
    int id = 0;
    bool answering = false;   // an answer (SREP) within the last 5 s
    double lastAnswer = -1;   // s since its last answer, -1 never
    double asksPerSec = 0, answersPerSec = 0;
    // A repeater that joined on its own (firmware 0x0032+): what it sends over the air, so one on a charger
    // shows too.
    bool wireless = false;
    std::string serial;       // its 8-hex short serial (the first half of "OHDG-..."), "" not known yet
    int board = -1;           // 0 Pro Micro, 1 Holyiot, 2 PCA10059, 3 Kuroshin
    int firmware = 0;
    int state = -1;           // its LED state: 0 looking for a main, 1 standby, 2 relaying, 3 weak signal
    int hearsMainRssi = 0;    // dBm it hears the main at, 0 unknown
    int heardRssi = 0;        // dBm the main hears it at, 0 unknown
  };
  std::vector<Repeater> repeaters;
  bool relaying = false;     // main: using its repeaters now (only while devices lose answers)
  int uselessFor = 0;        // main: s before a repeater that didn't help is asked again (0: none)
  // repeater: its id and its main (mainSerial: the main's serial, if it's plugged in here too)
  int id = 0;
  uint32_t mainBase = 0;
  std::string mainSerial;
  bool following = false;   // in sync with the main (else searching)
  int mainRssi = 0;         // dBm, 0 unknown
  int state = -1;           // its LED state (firmware 0x0032+): 0 looking for a main, 1 standby, 2 relaying, 3 weak signal
  std::array<double, 10> heardPerSec{};   // answers (RESPs) per second it hears, per slot of the main (DML_SLOTS)
  std::array<int, 10> heardRssi{};
};

}  // namespace lh
