// doorman-dongle-manager's window: the Doorman Dongles plugged in, their eight slots (what's paired,
// pairing into a slot, unpairing), the radio's live performance, its channels and firmware.
// Same look as openhouse-overlay (pixel font, sidebar pages, status bar).
#pragma once
#include <array>
#include <filesystem>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dongle.hpp"
#include "telemetry_client.hpp"

namespace openhouse::dongle {

class Ui {
 public:
  Ui();
  ~Ui();
  static void SetUpImGui(float k);
  void SetLogo(uint64_t texture) { logo_ = texture; }
  void SetScaleDrawer(std::function<void()> drawer) { drawScale_ = std::move(drawer); }
  void Poll(double now);
  void Draw(float width, float height);
  void SaveSettings();

 private:
  enum class Page { Overview, Slots, Radio, Channels, Timing, Repeater, Firmware, About };
  void Sidebar(float height);
  void StatusBar();
  void Overview(const DongleState& d);
  void Slots(const DongleState& d);
  void SlotCard(const DongleState& d, int slot, float width);
  void Radio(const DongleState& d);
  void Channels(const DongleState& d);
  void Timing(const DongleState& d);
  void Loadout(const DongleState& d, bool oldFw);
  void LiveCycle(const DongleState& d);
  void TimingDevices(const DongleState& d);
  void TimingForRepeater(const DongleState& d);
  void Repeater(const DongleState& d);
  void RepeaterMain(const DongleState& d);
  void RepeaterSelf(const DongleState& d);
  void RepeaterSpare(const DongleState& d);
  void DongleStrip(const DongleState& d);
  void PlacementPanel(bool tooFar);
  void RepeaterModals();
  void Link(const std::string& from, const std::string& main, int id);   // asks first
  void StartLink(const std::string& from, const std::string& main, int id);
  void StartReturn(const std::string& repeater);
  void PollRepeaters(const std::vector<DongleState>& all, double now);
  uint8_t ShownMask(const DongleState& d);
  bool SetUpFor(const std::string& serial);
  void Firmware(const DongleState& d);
  void About();
  void NoDongle();
  void PairBar(const DongleState& d);

  std::shared_ptr<Dongle> Current();
  Dongle::Done Report(const std::string& what);
  void Toast(const std::string& text, bool error = false, double seconds = 6);
  void Post(const std::string& text, bool ok, double seconds = 3);   // a toast from a dongle thread
  std::string DeviceLabel(const SlotState& s) const;
  void Remember(const DongleState& d);

  viewer::TelemetryClient driver_;
  Page page_ = Page::Overview;
  std::string dongle_;    // the selected dongle's serial
  int selectedSlot_ = 0;  // Radio page
  int pendingSlot_ = -1;  // slot a confirmation is about
  char rename_[64] = {};
  std::set<std::string> movedNames_;  // dongle/pairing id whose remembered name was put on the dongle
  // remembered devices, by pairing id: serial, model, nickname (settings file)
  struct Known { std::string serial, model, nickname; };
  std::map<std::string, Known> known_;
  bool knownDirty_ = false;   // known_ or keepModes_ changed: save the settings file
  // timing mode kept per dongle serial ("timing_modes" in the settings file), re-applied at connect
  std::map<std::string, int> keepModes_;
  std::map<std::string, int> notesSeen_;   // per dongle serial: DongleState::timingNotes toasted
  // per dongle serial, slot and mode in effect: mean loss this session (the Timing page's mode A/B)
  struct ModeMeans { std::array<double, kSlots> sum[4]{}; std::array<int, kSlots> n[4]{}; };
  std::map<std::string, ModeMeans> modeMeans_;
  // Repeater page
  std::string repeaterMain_;      // the main picked for "link this dongle as a repeater of"
  std::map<std::string, std::pair<uint8_t, double>> maskPending_;   // per main: switch mask set, until
  uint32_t infoOpen_ = 0;         // info blocks open (bits)
  bool placementOpen_ = false;
  double placeDoneAt_ = 0;        // set-up step 3 done (the repeater left USB and answers): when
  std::string linkFrom_, linkTo_, returnFor_;
  int linkId_ = -1;               // the repeater id the link confirm gives
  bool openLink_ = false, openReturn_ = false;
  // per main serial and slot: mean loss with a repeater switched on [0] and with none on [1] (the A/B)
  struct AbMeans { std::array<double, kSlots> sum[2]{}; std::array<int, kSlots> n[2]{}; };
  std::map<std::string, AbMeans> abMeans_;
  double abLast_ = 0;
  // results of actions, from the dongle threads (resultsMutex_ also guards repeaterMade*_)
  std::mutex resultsMutex_;
  struct Result { std::string text; bool ok; double seconds; };
  std::vector<Result> results_;
  std::string repeaterMade_;      // the dongle just linked as a repeater: set-up steps 3 (place) pending
  std::string repeaterMadeMain_;  // its main's serial
  int repeaterMadeId_ = 0;
  bool madeFresh_ = false;        // just linked: open the placement panel
  std::string toast_;
  bool toastError_ = false;
  double toastUntil_ = 0, now_ = 0;
  uint64_t logo_ = 0;
  std::function<void()> drawScale_;
  // firmware update (UF2 drive copy), on a thread of its own
  char firmwarePath_[512] = {};
  double lastSave_ = 0;
  std::thread updater_;
  std::atomic<bool> updating_{false};
  std::mutex updateMutex_;
  std::string updateStatus_;
  bool updateError_ = false;
  void StartUpdate(std::shared_ptr<Dongle> d, const std::string& file, bool nordicDfu);
  void StartBankUpdate(std::shared_ptr<Dongle> d, const std::filesystem::path& file);
  static std::filesystem::path PackagedBankImage(const std::string& name);
  // Last, so it goes first: its dongle threads report into the members above until they're joined.
  DongleManager manager_;
};

}  // namespace openhouse::dongle
