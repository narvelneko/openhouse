#include "platform/platform.hpp"
#include "ui.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>

#include "common/dongle_bank.hpp"
#include "common/nordic_dfu.hpp"
#include "common/ui_scale.hpp"
#include "common/ui_theme.hpp"
#include "lh/telemetry.hpp"
#include "lh/version.hpp"
#include "update_check.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace openhouse::dongle {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

ImFont* g_title = nullptr;
ImFont* g_huge = nullptr;

// openhouse-overlay's colours
const ImVec4 kGood{0.36f, 0.80f, 0.45f, 1}, kWarn{1.00f, 0.78f, 0.30f, 1}, kBad{1.00f, 0.42f, 0.40f, 1},
    kInfo{0.45f, 0.70f, 1.00f, 1}, kOff{0.55f, 0.58f, 0.62f, 1}, kAccent{1.00f, 0.86f, 0.55f, 1};

fs::path SettingsPath() { return lh::platform::FsPath(lh::platform::ConfigDirectory()) / "dongle.json"; }

// Timing modes (0x2D): one word each; any mode the app doesn't offer is "Custom".
const char* ModeWord(int m) { return m == 1 ? "Classic" : m == 3 ? "Adaptive" : m >= 0 ? "Custom" : "?"; }
const char* ModeUse(int m) { return m == 3 ? "Everyday use" : "Troubleshooting"; }   // the line on a mode card
const char* ModeLabel(int m) { return m == 1 ? "1 Classic" : m == 3 ? "3 Adaptive" : ModeWord(m); }

ImU32 U32(const ImVec4& c, float alpha = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

const char* ModeTag(int m) { return m == kDefaultTimingMode ? "DEFAULT" : nullptr; }   // a mode's badge

// A small pill at p (top left): the text in a colour on a dim fill of that colour. Returns its size.
ImVec2 BadgeSize(const char* text) {
  const float fs = ImGui::GetFontSize();
  const ImVec2 t = ImGui::CalcTextSize(text);
  return ImVec2(t.x + fs * 0.8f, t.y + fs * 0.2f);
}
ImVec2 Badge(ImDrawList* dl, ImVec2 p, const char* text, const ImVec4& c, float alpha = 1) {
  const float fs = ImGui::GetFontSize();
  const ImVec2 s = BadgeSize(text);
  dl->AddRectFilled(p, ImVec2(p.x + s.x, p.y + s.y), U32(c, 0.2f * alpha), fs * 0.3f);
  dl->AddText(ImVec2(p.x + fs * 0.4f, p.y + fs * 0.1f), U32(c, alpha), text);
  return s;
}

// A status dot; `frame`: centred on a frame-height row (after AlignTextToFramePadding).
void Dot(const ImVec4& color, float radius = 5, bool frame = false) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = frame ? ImGui::GetFrameHeight() : ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius, p.y + h / 2), radius, U32(color));
  ImGui::Dummy(ImVec2(radius * 2 + 4, frame ? ImGui::GetTextLineHeight() : h));
  ImGui::SameLine();
}

// A page's title: large, with its icon before it.
void Title(const char* text, lh::ui::Icon icon) {
  ImGui::PushFont(g_title);
  const float s = ImGui::GetTextLineHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  lh::ui::DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + s / 2, p.y + s / 2), s * 0.85f, lh::ui::kAccent);
  ImGui::Dummy(ImVec2(s, s));
  ImGui::SameLine(0, s * 0.45f);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
}

void Title(const char* text) { Title(text, lh::ui::Icon::Dongle); }

void Tip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

bool BigButton(const char* label, float width = 0) { return ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.25f)); }

// A button tinted with a colour: the main action, or a dangerous one.
void PushTint(const ImVec4& c) {   // three colours: pop 3
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 0.6f, c.y * 0.6f, c.z * 0.6f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.75f, c.y * 0.75f, c.z * 0.75f, 1));
}
bool TintButton(const char* label, const ImVec4& c, float width = 0) {
  PushTint(c);
  const bool r = BigButton(label, width);
  ImGui::PopStyleColor(3);
  return r;
}

// A confirmation. body (optional) draws above the buttons instead of text; yes names the action.
bool Confirm(const char* id, const char* text, const char* yes_label = "Yes", const std::function<void()>& body = {},
             bool can = true) {
  bool yes = false;
  if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    if (body) body();
    else ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const float w = ImGui::GetFontSize() * 7, h = ImGui::GetFrameHeight() * 1.5f;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button(yes_label, ImVec2(w, h))) { yes = true; ImGui::CloseCurrentPopup(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(w, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return yes;
}

// A summary tile: a big value and what it is.
void Tile(const char* id, const char* value, const char* label, const ImVec4& color, float width) {
  const ImGuiStyle& st = ImGui::GetStyle();
  ImGui::PushFont(g_huge);
  const float big = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  const float h = big + ImGui::GetTextLineHeight() + st.ItemSpacing.y + st.WindowPadding.y * 2 + 2;
  ImGui::BeginChild(id, ImVec2(width, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  ImGui::PushFont(g_huge);
  ImGui::TextColored(color, "%s", value);
  ImGui::PopFont();
  ImGui::TextDisabled("%s", label);
  ImGui::EndChild();
}

ImVec4 LossColor(double loss) {
  if (loss < 0) return kOff;
  return loss < 3 ? kGood : loss < 10 ? kWarn : kBad;
}

ImVec4 RssiColor(int rssi) {
  if (!rssi) return kOff;
  return rssi > -65 ? kGood : rssi > -78 ? kWarn : kBad;
}

const char* SignalWord(int rssi) {
  if (!rssi) return "-";
  return rssi > -55 ? "excellent" : rssi > -65 ? "good" : rssi > -78 ? "fair" : "weak";
}

int Bars(int rssi) { return !rssi ? 0 : rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -78 ? 2 : 1; }

// Four signal bars at p (top left), h high, coloured by strength. Returns the width with the gap after.
float DrawBars(ImDrawList* dl, ImVec2 p, float h, int rssi) {
  const float w = h * 0.22f, gap = h * 0.12f;
  const int bars = Bars(rssi);
  for (int i = 0; i < 4; ++i) {
    const float bh = h * (0.35f + 0.65f * float(i + 1) / 4);
    const ImVec2 a(p.x + float(i) * (w + gap), p.y + h - bh), b(a.x + w, p.y + h);
    dl->AddRectFilled(a, b, i < bars ? U32(RssiColor(rssi)) : U32(kOff, 0.3f), 1);
  }
  return 4 * (w + gap);
}
void SignalBars(int rssi) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight();
  p.y += h * 0.3f;    // sit on the text's baseline, not the line's top
  ImGui::Dummy(ImVec2(DrawBars(ImGui::GetWindowDrawList(), p, h, rssi), h));
}

// A small graph with its current value written on it.
void Graph(const char* id, const std::deque<float>& h, float lo, float hi, const ImVec4& color, const char* overlay,
           float width, float height) {
  std::vector<float> v(h.begin(), h.end());
  if (v.empty()) v.push_back(lo);
  ImGui::PushStyleColor(ImGuiCol_PlotLines, color);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.07f, 0.09f, 1));
  ImGui::PlotLines(id, v.data(), int(v.size()), 0, overlay, lo, hi, ImVec2(width, height));
  ImGui::PopStyleColor(2);
}

std::string Uptime(uint32_t ms) {
  const uint32_t s = ms / 1000;
  char t[48];
  if (s >= 3600) std::snprintf(t, sizeof t, "%uh %02um", s / 3600, s / 60 % 60);
  else std::snprintf(t, sizeof t, "%um %02us", s / 60, s % 60);
  return t;
}

// Folders of mounted UF2 drives (the dongle's update mode shows up as one).
std::vector<fs::path> Uf2Drives() {
  std::vector<fs::path> out;
  std::error_code ec;
#ifdef _WIN32
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i)
    if (mask & (1u << i)) {
      const fs::path root = std::string(1, char('A' + i)) + ":\\";
      if (fs::exists(root / "INFO_UF2.TXT", ec)) out.push_back(root);
    }
#else
  const char* user = std::getenv("USER");
  std::vector<fs::path> bases = {"/media", "/mnt"};
  if (user) { bases.push_back(fs::path("/run/media") / user); bases.push_back(fs::path("/media") / user); }
  for (const auto& base : bases) {
    if (!fs::is_directory(base, ec)) continue;
    for (const auto& e : fs::directory_iterator(base, ec))
      if (fs::exists(e.path() / "INFO_UF2.TXT", ec)) out.push_back(e.path());
  }
#endif
  return out;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
Ui::Ui() : driver_(lh::telemetry::kDefaultPort) {
  std::ifstream in(SettingsPath());
  const json j = json::parse(in, nullptr, false);
  // Kept in a variable: items() refers to the object it came from.
  const json devices = j.is_object() ? j.value("devices", json::object()) : json::object();
  if (devices.is_object())
    for (const auto& [id, v] : devices.items())
      known_[id] = {v.value("serial", ""), v.value("model", ""), v.value("nickname", "")};
  // timing mode pinned per dongle: {"OHDG-...": 1}
  const json modes = j.is_object() ? j.value("timing_modes", json::object()) : json::object();
  if (modes.is_object())
    for (const auto& [serial, v] : modes.items())
      if (v.is_number_integer() && TimingModeOffered(v.get<int>())) keepModes_[serial] = v.get<int>();
  if (const char* page = std::getenv("OPENHOUSE_DONGLE_PAGE")) {   // testing, with the snapshot
    const std::string p = page;
    page_ = p == "slots" ? Page::Slots : p == "radio" ? Page::Radio : p == "channels" ? Page::Channels
          : p == "timing" ? Page::Timing : p == "repeater" ? Page::Repeater : p == "firmware" ? Page::Firmware
          : p == "about" ? Page::About : Page::Overview;
  }
}

Ui::~Ui() {
  if (updater_.joinable()) updater_.join();
}

void Ui::SaveSettings() {
  json devices = json::object();
  for (const auto& [id, k] : known_) devices[id] = {{"serial", k.serial}, {"model", k.model}, {"nickname", k.nickname}};
  std::error_code ec;
  fs::create_directories(SettingsPath().parent_path(), ec);
  json modes = json::object();
  for (const auto& [serial, m] : keepModes_) modes[serial] = m;
  std::ofstream(SettingsPath()) << json{{"devices", devices}, {"timing_modes", modes}}.dump(2) << "\n";
  knownDirty_ = false;
}

void Ui::SetUpImGui(float k) {
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  static ImFont *bodyBmp = nullptr, *titleBmp = nullptr, *hugeBmp = nullptr;
  static ImFont *bodyVec = nullptr, *titleVec = nullptr, *hugeVec = nullptr;
  if (io.Fonts->Fonts.empty()) {
    ImFontConfig bmp;
    bmp.OversampleH = bmp.OversampleV = 1;
    bmp.PixelSnapH = true;
    bmp.SizePixels = 13.f;
    bodyBmp = io.Fonts->AddFontDefaultBitmap(&bmp);
    bmp.SizePixels = 26.f;
    titleBmp = io.Fonts->AddFontDefaultBitmap(&bmp);
    bmp.SizePixels = 39.f;
    hugeBmp = io.Fonts->AddFontDefaultBitmap(&bmp);

    ImFontConfig vec;
    vec.PixelSnapH = true;
    vec.SizePixels = 13.f;
    bodyVec = io.Fonts->AddFontDefaultVector(&vec);
    vec.SizePixels = 26.f;
    titleVec = io.Fonts->AddFontDefaultVector(&vec);
    vec.SizePixels = 39.f;
    hugeVec = io.Fonts->AddFontDefaultVector(&vec);
  }
  const bool integerScale = std::fabs(k - std::round(k)) < 0.01f;
  io.FontDefault = integerScale ? bodyBmp : bodyVec;
  g_title = integerScale ? titleBmp : titleVec;
  g_huge = integerScale ? hugeBmp : hugeVec;
  ImGui::GetStyle() = ImGuiStyle();
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0;
  style.FramePadding = ImVec2(8, 6);
  style.ItemSpacing = ImVec2(10, 8);
  style.WindowPadding = ImVec2(12, 12);
  style.ScaleAllSizes(k);
  style.FontScaleMain = k;
  lh::ui::ApplyTheme(k);
  style.Colors[ImGuiCol_ChildBg] = lh::ui::CardColor();   // bordered panels are cards over the snow
}

std::shared_ptr<Dongle> Ui::Current() {
  const auto& all = manager_.Dongles();
  for (const auto& d : all)
    if (d->Serial() == dongle_) return d;
  if (all.empty()) return nullptr;
  dongle_ = all.front()->Serial();
  return all.front();
}

void Ui::Toast(const std::string& text, bool error, double seconds) {
  toast_ = text;
  toastError_ = error;
  toastUntil_ = now_ + seconds;
}

void Ui::Post(const std::string& text, bool ok, double seconds) {
  std::lock_guard lock(resultsMutex_);
  results_.push_back({text, ok, seconds});
}

Dongle::Done Ui::Report(const std::string& what) {
  return [this, what](bool ok, const std::string& message) {
    std::lock_guard lock(resultsMutex_);
    results_.push_back({ok ? message : what + " failed: " + message, ok, 6});
  };
}

std::string Ui::DeviceLabel(const SlotState& s) const {
  if (!s.nickname.empty()) return s.nickname;
  if (!s.id.model.empty() && !s.id.serial.empty()) return s.id.model + "  " + s.id.serial;
  if (!s.id.serial.empty()) return s.id.serial;
  return s.used ? "Unknown device" : "Empty";
}

// What we learn about a slot's device is remembered by its pairing id, and given back to
// every dongle (a device keeps its id when paired elsewhere).
void Ui::Remember(const DongleState& d) {
  for (const auto& s : d.slots) {
    if (s.pairId.empty() || s.id.serial.empty()) continue;
    auto& k = known_[s.pairId];
    if (k.serial != s.id.serial || (!s.id.model.empty() && k.model != s.id.model)) {
      k.serial = s.id.serial;
      if (!s.id.model.empty()) k.model = s.id.model;
      knownDirty_ = true;
    }
  }
}

void Ui::Poll(double now) {
  now_ = now;
  manager_.Poll(Now());
  driver_.Poll(now);
  {
    std::lock_guard lock(resultsMutex_);
    for (const auto& r : results_) Toast(r.text, !r.ok, r.seconds);
    results_.clear();
  }
  // Names: the driver's (it knows which device is on which slot), then what we remember.
  std::map<std::string, std::string> driverModel;
  if (driver_.Connected())
    for (const auto& dev : driver_.Current().devices) driverModel[dev.serial] = dev.model;
  std::vector<DongleState> snapshots;
  for (const auto& d : manager_.Dongles()) {
    d->SetAutoIdentify(!driver_.Connected());
    const auto keep = keepModes_.find(d->Serial());
    d->SetKeepMode(keep != keepModes_.end() && TimingModeOffered(keep->second) ? keep->second : -1);   // other pins: ignored
    const DongleState st = d->Snapshot();
    snapshots.push_back(st);
    int& seen = notesSeen_[st.serial];
    if (st.timingNotes > seen) Toast(st.timingNote, st.timingNoteError);   // what it did by itself
    seen = st.timingNotes;   // back to 0 when it's plugged in again
    Remember(st);
    for (int i = 0; i < st.slotCount; ++i) {
      const auto& s = st.slots[i];
      if (s.pairId.empty()) continue;
      Identity id;
      if (driver_.Connected())
        for (const auto& r : driver_.Current().receivers)
          if (r.serial == st.serial + "/" + std::to_string(i) && !r.connected.empty()) {
            id = {r.connected, driverModel.count(r.connected) ? driverModel[r.connected] : "", "driver"};
          }
      const auto k = known_.find(s.pairId);
      if (id.serial.empty() && k != known_.end() && !k->second.serial.empty())
        id = {k->second.serial, k->second.model, "remembered"};
      if (!id.serial.empty() && id.serial != s.id.serial) d->SetKnown(s.pairId, id);
      if (!st.names) {   // older firmware: the name lives in our settings only
        if (k != known_.end() && k->second.nickname != s.nickname) d->SetNickname(i, k->second.nickname);
      } else if (s.nameRead && s.nickname.empty() && k != known_.end() && !k->second.nickname.empty() &&
                 movedNames_.insert(st.serial + "/" + s.pairId).second) {
        d->SetName(i, k->second.nickname, nullptr);   // a name from before the dongle kept them: moved onto it
      }
    }
  }
  PollRepeaters(snapshots, now);
  if (knownDirty_ && now - lastSave_ > 2.0) {  // a file write: not every frame
    SaveSettings();
    lastSave_ = now;
  }
}

// ---- layout ----

void Ui::Draw(float width, float height) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::Begin("Doorman Dongle", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
  lh::ui::Snow(ImGui::GetWindowDrawList(), ImVec2(0, 0), ImVec2(width, height));
  const float statusHeight = ImGui::GetFrameHeightWithSpacing() + 4 * lh::ui::Scale();
  const float body = ImGui::GetContentRegionAvail().y - statusHeight;
  Sidebar(body);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("page", ImVec2(0, body), ImGuiChildFlags_None);
  ImGui::PopStyleColor();
  const auto dongle = Current();
  if (page_ == Page::About) About();
  else if (!dongle) NoDongle();
  else {
    const DongleState d = dongle->Snapshot();
    if (!d.open && !d.error.empty()) {
      Title(d.serial.c_str());
      ImGui::TextColored(kBad, "Can't use this dongle: %s", d.error.c_str());
    } else {
      switch (page_) {
        case Page::Overview: Overview(d); break;
        case Page::Slots: Slots(d); break;
        case Page::Radio: Radio(d); break;
        case Page::Channels: Channels(d); break;
        case Page::Timing: Timing(d); break;
        case Page::Repeater: Repeater(d); break;
        case Page::Firmware: Firmware(d); break;
        case Page::About: break;
      }
    }
  }
  ImGui::EndChild();
  StatusBar();
  ImGui::End();
}

namespace {
// What a plugged-in dongle is: the sidebar's list and the Repeater page's strip tell them apart by it.
// Roles have fixed colours, apart from the health colours: dongles blue (with devices or not), repeaters warm.
enum class Role { Main, Repeater, Spare };
Role RoleOf(const DongleState& s) {
  if (s.haveRole && s.role == 1) return Role::Repeater;
  return s.paired > 0 || s.satMask ? Role::Main : Role::Spare;
}
ImVec4 RoleColor(const DongleState& s) {
  const Role r = RoleOf(s);
  return r == Role::Repeater ? kAccent : kInfo;
}
const char* RoleWord(Role r) { return r == Role::Main ? "MAIN" : r == Role::Repeater ? "REPEATER" : "SPARE"; }

// A main's repeater socket. The main only asks a repeater for what it missed, so on a clean link nothing
// is asked and nothing answered: that's Standby (fine), not a fault.
enum class Sock { Empty, Off, Live, Standby, Weak, Lost };
Sock SocketState(const DongleState::RelaySat& r, bool on, bool seen) {
  if (!on) return seen ? Sock::Off : Sock::Empty;
  if (r.msSinceSrep < 3000) return Sock::Live;
  if (r.asksPerSec < 0.5) return Sock::Standby;
  if (r.msSinceSrep <= 5000) return Sock::Weak;
  return Sock::Lost;
}
// A socket with what a repeater that joined on its own says of itself: weak signal (either way at -80 dBm or
// worse, or it says so), live while the main relays and it answers, else standby. A gone one isn't in the mask
// any more: the socket empties.
Sock SockFor(const DongleState& d, int k, uint8_t mask, uint8_t known) {
  const Sock st = SocketState(d.relaySats[k], mask & (1u << k), known & (1u << k));
  if (!(d.autoMask >> k & 1) || st == Sock::Lost || st == Sock::Off || st == Sock::Empty) return st;
  const auto& x = d.autoReps[k];
  const int w = std::min(x.hearsMainRssi ? x.hearsMainRssi : 0, x.heardRssi ? x.heardRssi : 0);
  const auto& r = d.relaySats[k];
  const bool fewAnswers = d.relaying && r.asksPerSec >= 5 && r.srepsPerSec < 0.75 * r.asksPerSec;   // (at -70 dBm: 55-65 %)
  if (x.state == 3 || (w && w <= -75) || d.uselessFor || fewAnswers) return Sock::Weak;   // (useless: it hears too little of the devices)
  if (d.relaying && d.relaySats[k].msSinceSrep < 3000) return Sock::Live;
  return Sock::Standby;
}
const char* BoardWord(int board) {
  switch (board) { case 0: return "Pro Micro"; case 1: return "Holyiot"; case 2: return "PCA10059"; case 3: return "Kuroshin"; default: return "Dongle"; }
}
ImVec4 SockColor(Sock s) {
  switch (s) {
    case Sock::Live: return kGood;
    case Sock::Standby: return kInfo;
    case Sock::Weak: return kWarn;
    case Sock::Lost: return kBad;
    default: return kOff;
  }
}
const char* SockWord(Sock s) {
  switch (s) {
    case Sock::Live: return "LIVE";
    case Sock::Standby: return "STANDBY";
    case Sock::Weak: return "WEAK";
    case Sock::Lost: return "NO ANSWER";
    case Sock::Off: return "OFF";
    default: return "Empty";
  }
}
std::string RName(int id) { return "R" + std::to_string(id + 1); }   // ids 0 and 1 are shown as R1 and R2
}  // namespace

void Ui::Sidebar(float height) {
  ImGui::PushFont(g_title);
  const float name = ImGui::CalcTextSize("openhouse").x;
  ImGui::PopFont();
  const float logoSide = ImGui::GetFontSize() * 2.6f;
  const float w = std::max(ImGui::GetFontSize() * 11, logoSide + name + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().WindowPadding.x * 2 + 4);
  ImGui::BeginChild("sidebar", ImVec2(w, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  if (logo_) {
    ImGui::Image(static_cast<ImTextureID>(logo_), ImVec2(logoSide, logoSide));
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted("openhouse");
  ImGui::PopFont();
  ImGui::TextDisabled("dongle manager");
  ImGui::EndGroup();
  ImGui::Spacing();
  // which dongle (more than one can be plugged in)
  const auto& all = manager_.Dongles();
  if (all.size() > 1) {
    // each entry: the role's colour dot (the Repeater page's strip is the legend), then role and serial
    const auto label = [](const DongleState& s) { return std::string("  ") + BoardWord(s.board) + " " + ShortSerial(s.serial); };   // (the dot's color is its role)
    const float r = ImGui::GetFontSize() * 0.26f;
    std::string preview = dongle_;
    ImVec4 previewColor = kOff;
    for (const auto& d : all)
      if (d->Serial() == dongle_) { const auto s = d->Snapshot(); preview = label(s); previewColor = RoleColor(s); }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(-1);
    const bool open = ImGui::BeginCombo("##dongle", preview.c_str());
    dl->AddCircleFilled(ImVec2(at.x + ImGui::GetStyle().FramePadding.x + r, at.y + ImGui::GetFrameHeight() / 2), r, U32(previewColor));
    if (open) {
      for (const auto& d : all) {
        const auto s = d->Snapshot();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable((label(s) + "##" + s.serial).c_str(), s.serial == dongle_)) dongle_ = s.serial;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() / 2), r, U32(RoleColor(s)));
      }
      ImGui::EndCombo();
    }
  } else if (all.size() == 1) {
    ImGui::TextDisabled("%s", all.front()->Serial().c_str());
  }
  ImGui::Separator();
  ImGui::Spacing();
  using lh::ui::Icon;
  struct Item { Page page; const char* label; Icon icon; };
  static const Item items[] = {{Page::Overview, "Overview", Icon::Overview}, {Page::Slots, "Slots", Icon::Slots},
                               {Page::Radio, "Radio", Icon::Wireless},       {Page::Channels, "Channels", Icon::Channels},
                               {Page::Timing, "Timing", Icon::Timing},       {Page::Repeater, "Repeater", Icon::Repeater},
                               {Page::Firmware, "Firmware", Icon::Chip},     {Page::About, "About", Icon::About}};
  const float rowHeight = ImGui::GetFrameHeight() * 1.3f;
  // Timing, Repeater: only for firmware that has them (asked from the dongle, DongleState)
  const auto cur = Current();
  const DongleState s = cur ? cur->Snapshot() : DongleState{};
  const bool extras = s.extras;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4 * lh::ui::Scale()));
  // the Repeater item's dot: red when a repeater of this main doesn't answer, blue while a set-up is pending
  ImVec4 repeaterDot{0, 0, 0, 0};
  if (cur) {
    if (s.haveRole && s.role == 0)
      for (int k = 0; k < kMaxRepeaters; ++k)
        if (SocketState(s.relaySats[k], s.satMask & (1u << k), true) == Sock::Lost) repeaterDot = kBad;
    std::lock_guard lock(resultsMutex_);
    if (repeaterDot.w == 0 && !repeaterMade_.empty()) repeaterDot = kInfo;
  }
  for (const auto& item : items) {
    if ((item.page == Page::Timing || item.page == Page::Repeater) && !extras && page_ != item.page) continue;
    if (lh::ui::NavItem(item.label, item.icon, page_ == item.page, rowHeight)) page_ = item.page;
    if (item.page == Page::Repeater && repeaterDot.w > 0) {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(b.x - ImGui::GetFontSize() * 0.9f, (a.y + b.y) / 2), ImGui::GetFontSize() * 0.3f,
                                                  U32(repeaterDot));
    }
  }
  ImGui::PopStyleVar();
  // the dongle and the driver, at the bottom
  const float line = ImGui::GetTextLineHeightWithSpacing();
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - line * 3.4f));
  ImGui::Separator();
  Dot(s.open ? kGood : kBad);
  ImGui::TextUnformatted(s.open ? "Dongle connected" : "No dongle");
  Dot(driver_.Connected() ? kGood : kOff);
  ImGui::TextUnformatted(driver_.Connected() ? "Driver running" : "Driver not running");
  ImGui::EndChild();
}

void Ui::StatusBar() {
  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  if (!toast_.empty() && now_ < toastUntil_) ImGui::TextColored(toastError_ ? kBad : kGood, "%s", toast_.c_str());
  else ImGui::TextDisabled("openhouse %s", lh::kVersion);
  if (drawScale_) {
    const float w = lh::ui::ScaleComboWidth();
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w));
    drawScale_();
  }
}

void Ui::NoDongle() {
  Title("No Doorman Dongle");
  ImGui::TextWrapped("Plug in a Doorman Dongle");
#ifndef _WIN32
  ImGui::Spacing();
  ImGui::TextDisabled("Needs the openhouse udev rule (see the README)");
#endif
}

// The pairing banner: a countdown while the dongle is pairing, else the button.
void Ui::PairBar(const DongleState& d) {
  const auto dongle = Current();
  if (d.pairing) {
    const double left = d.pairUntil > 0 ? std::max(0.0, d.pairUntil - Now()) : -1;
    char text[128];
    std::snprintf(text, sizeof text, "Pairing into %s%s",
                  d.pairTarget >= 0 ? ("slot " + std::to_string(d.pairTarget)).c_str() : "the first free slot",
                  left >= 0 ? ("  -  " + std::to_string(int(left)) + " s left").c_str() : "");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.18f, 0.28f, 1));
    // One row: the title and the button, centred on each other; the height fits it at any scale.
    ImGui::PushFont(g_title);
    const float titleH = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const float rowH = std::max(titleH, ImGui::GetFrameHeight());
    const float padY = ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("pairing", ImVec2(0, rowH + 2 * padY), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float y0 = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y0 + (rowH - titleH) * 0.5f);
    ImGui::PushFont(g_title);   // first: the dot centres on the title's line
    Dot(kInfo, 6);
    ImGui::TextColored(kInfo, "%s", text);
    ImGui::PopFont();
    const float bw = ImGui::GetFontSize() * 9;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
    ImGui::SetCursorPosY(y0 + (rowH - ImGui::GetFrameHeight()) * 0.5f);
    if (ImGui::Button("Stop pairing", ImVec2(bw, 0))) dongle->StopPairing(Report("Stop pairing"));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return;
  }
  int firstFree = -1;
  for (int i = 0; i < d.slotCount && firstFree < 0; ++i)
    if (!d.slots[i].used) firstFree = i;
  if (firstFree >= 0) {
    if (TintButton("Pair a device", kInfo, ImGui::GetFontSize() * 12)) dongle->Pair(60, -1, Report("Pairing"));
    Tip("60 s of pairing into the first free slot");
  } else {
    ImGui::BeginDisabled();
    BigButton("All slots paired", ImGui::GetFontSize() * 12);
    ImGui::EndDisabled();
    Tip("Pairing from a slot's card replaces its device");
  }
}

// ---- pages ----

void Ui::Overview(const DongleState& d) {
  Title("Overview", lh::ui::Icon::Overview);
  PairBar(d);
  ImGui::Spacing();
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float tw = (ImGui::GetContentRegionAvail().x - gap * 3) / 4;
  double loss = 0, corrupted = 0, unanswered = 0;
  int n = 0, worstRssi = 0;
  for (const auto& s : d.slots)
    if (s.active) {
      if (s.lossPercent >= 0) { loss += s.lossPercent; n++; }
      if (s.rssiDongle && (!worstRssi || s.rssiDongle < worstRssi)) worstRssi = s.rssiDongle;
      corrupted += s.crcPerSec; unanswered += s.timeoutsPerSec;
    }
  char v[32];
  std::snprintf(v, sizeof v, "%d / %d", d.joined, d.slotCount);
  Tile("t1", v, "devices connected", d.joined ? kGood : kOff, tw);
  ImGui::SameLine();
  std::snprintf(v, sizeof v, "%d", d.paired);
  Tile("t2", v, "slots paired", kInfo, tw);
  ImGui::SameLine();
  std::snprintf(v, sizeof v, "%.1f kB/s", d.kBytesPerSec);
  Tile("t3", v, "data from devices", kAccent, tw);
  ImGui::SameLine();
  if (n) std::snprintf(v, sizeof v, "%.1f %%", loss / n); else std::snprintf(v, sizeof v, "-");
  Tile("t4", v, "radio loss (resent)", n ? LossColor(loss / n) : kOff, tw);
  // The link full rather than the radio bad: a lot of data, loss everywhere, and answers missing rather
  // than corrupted. The channels can't fix that; fewer devices on this dongle can.
  if (d.haveTiming) {
    ImGui::Spacing();
    char line[160];
    int at = std::snprintf(line, sizeof line, "Answers %d B", d.answerBytes);
    if (d.largeAnswers && d.normalAnswerBytes) at += std::snprintf(line + at, sizeof line - at, " (large, normally %d)", d.normalAnswerBytes);
    at += std::snprintf(line + at, sizeof line - at, "  |  cycle %.1f ms", d.cycleUs / 1000.0);
    if (d.saturationPercent >= 0) std::snprintf(line + at, sizeof line - at, "  |  saturation %d %%", d.saturationPercent);
    ImGui::TextDisabled("%s", line);
  }
  // Saturated even with large answers (or a dongle that can't tell), loss everywhere, answers missing rather
  // than corrupted: the link is full. The channels can't fix that; fewer devices on this dongle can.
  const bool full = d.saturationPercent >= 0 ? d.largeAnswers && d.saturationPercent >= 50
                                             : d.kBytesPerSec >= 110;
  if (n && full && loss / n >= 20 && corrupted < 0.1 * unanswered) {
    ImGui::Spacing();
    ImGui::TextColored(kWarn, "Link full: move some devices to a second dongle");
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Slots");
  if (ImGui::BeginTable("slots", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
    ImGui::TableSetupColumn("Device");
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8);
    ImGui::TableSetupColumn("Loss", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
    ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
    ImGui::TableHeadersRow();
    for (int i = 0; i < d.slotCount; ++i) {
      const auto& s = d.slots[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%d", i);
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.used) ImGui::TextUnformatted(DeviceLabel(s).c_str()); else ImGui::TextDisabled("Empty");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (!s.used) ImGui::TextDisabled("-");
      else { Dot(s.active ? kGood : kOff, 5, true); ImGui::TextColored(s.active ? kGood : kOff, s.active ? "Connected" : "Off"); }
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.active) { SignalBars(s.rssiDongle); ImGui::SameLine(); ImGui::TextColored(RssiColor(s.rssiDongle), "%d dBm", s.rssiDongle); }
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.active && s.lossPercent >= 0) ImGui::TextColored(LossColor(s.lossPercent), "%.1f %%", s.lossPercent); else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.active) ImGui::Text("%.1f kB/s", s.kBytesPerSec); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  int on = 0;
  for (int k = 0; k < kHops; ++k) on += (d.channelMask >> k) & 1;
  ImGui::TextDisabled("Radio on %d of %d channels, now at %d MHz  |  firmware %d  |  up %s", on, kHops, 2400 + d.currentChannel,
                      d.firmware, Uptime(d.uptimeMs).c_str());
  if (d.extras && d.haveTiming) {   // "Timing mode 3  Adaptive  [DEFAULT]"
    if (TimingModeOffered(d.modeNow)) ImGui::TextDisabled("Timing mode %d", d.modeNow);
    else ImGui::TextDisabled("Timing");
    ImGui::SameLine();
    ImGui::TextUnformatted(ModeWord(d.modeNow));
    if (const char* tag = ModeTag(d.modeNow)) {
      ImGui::SameLine();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const ImVec2 bs = Badge(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y - ImGui::GetFontSize() * 0.1f), tag, kGood);
      ImGui::Dummy(ImVec2(bs.x, ImGui::GetTextLineHeight()));
    }
  }
}

void Ui::SlotCard(const DongleState& d, int i, float width) {
  const auto dongle = Current();
  const auto& s = d.slots[i];
  const ImVec4 edge = !s.used ? kOff : s.active ? kGood : kWarn;
  ImGui::PushID(i);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(edge.x, edge.y, edge.z, s.used ? 0.7f : 0.25f));
  // header (title font) + separator + four lines + the buttons
  ImGui::PushFont(g_title);
  const float titleH = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  const ImGuiStyle& st = ImGui::GetStyle();
  const float cardH = st.WindowPadding.y * 2 + titleH + st.ItemSpacing.y * 7 + ImGui::GetTextLineHeight() * 4 +
                      ImGui::GetFrameHeight() * 1.25f + 2;
  ImGui::BeginChild("card", ImVec2(width, cardH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  // header: slot number and state
  ImGui::PushFont(g_title);
  ImGui::Text("Slot %d", i);
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 7);
  Dot(edge);
  ImGui::TextColored(edge, "%s", !s.used ? "Empty" : s.active ? "Connected" : "Off");
  ImGui::Separator();
  if (!s.used) {
    ImGui::TextDisabled("Nothing paired");
    ImGui::Spacing();
    ImGui::BeginDisabled(d.pairing);
    if (TintButton("Pair to this slot", kInfo, -1)) dongle->Pair(60, i, Report("Pairing"));
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopID();
    return;
  }
  ImGui::TextUnformatted(DeviceLabel(s).c_str());
  if (!s.nickname.empty() && !s.id.serial.empty()) ImGui::TextDisabled("%s %s", s.id.model.c_str(), s.id.serial.c_str());
  else ImGui::TextDisabled("radio id %s%s", s.pairId.c_str(), s.id.source.empty() ? "" : ("  (" + s.id.source + ")").c_str());
  if (s.active) {
    SignalBars(s.rssiDongle);
    ImGui::SameLine();
    ImGui::TextColored(RssiColor(s.rssiDongle), "%d dBm %s", s.rssiDongle, SignalWord(s.rssiDongle));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextColored(LossColor(s.lossPercent), "%.1f %% loss", std::max(0.0, s.lossPercent));
    ImGui::Text("%.1f kB/s   IMU %.0f Hz   light %.0f/s", s.kBytesPerSec, s.imuHz, s.lightPerSec);
  } else {
    ImGui::TextDisabled("Turned off or out of range");
    ImGui::TextDisabled(" ");
  }
  // actions
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() * 1.25f - ImGui::GetStyle().WindowPadding.y);
  const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
  ImGui::BeginDisabled(d.pairing);
  if (BigButton("Replace", bw)) { pendingSlot_ = i; ImGui::OpenPopup("Replace?"); }
  ImGui::EndDisabled();
  Tip("Replaces the device in this slot");
  ImGui::SameLine();
  if (TintButton("Unpair", kBad, bw)) { pendingSlot_ = i; ImGui::OpenPopup("Unpair?"); }
  
  ImGui::SameLine();
  if (BigButton("More...", bw)) {
    pendingSlot_ = i;
    std::snprintf(rename_, sizeof rename_, "%s", s.nickname.c_str());
    ImGui::OpenPopup("slot menu");
  }
  if (ImGui::BeginPopup("slot menu")) {
    ImGui::BeginDisabled(!s.active);
    if (ImGui::MenuItem("Turn off")) dongle->PowerOff(i, Report("Turn off"));
    if (ImGui::MenuItem("Identify (read its name)")) dongle->Identify(i, Report("Identify"));
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextDisabled("Name");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::InputText("##name", rename_, sizeof rename_, ImGuiInputTextFlags_EnterReturnsTrue)) {
      known_[s.pairId].nickname = rename_;
      knownDirty_ = true;
      if (dongle->Snapshot().names) dongle->SetName(i, rename_, Report("Rename"));
      else dongle->SetNickname(i, rename_);
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("Enter to save");
    ImGui::EndPopup();
  }
  char text[160];
  std::snprintf(text, sizeof text, "Pair a new device into slot %d? %s will be forgotten.", i, DeviceLabel(s).c_str());
  if (Confirm("Replace?", text) && pendingSlot_ == i) dongle->Pair(60, i, Report("Pairing"));
  std::snprintf(text, sizeof text, "Unpair %s from slot %d? It can't connect until it's paired again.", DeviceLabel(s).c_str(), i);
  if (Confirm("Unpair?", text) && pendingSlot_ == i) dongle->Forget(i, Report("Unpair"));
  ImGui::EndChild();
  ImGui::PopID();
}

void Ui::Slots(const DongleState& d) {
  Title("Slots", lh::ui::Icon::Slots);
  PairBar(d);
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 10);
  if (!d.pairing) {
    ImGui::BeginDisabled(d.paired == 0);
    if (TintButton("Unpair all", kBad, ImGui::GetFontSize() * 10)) ImGui::OpenPopup("Unpair all?");
    ImGui::EndDisabled();
  }
  if (Confirm("Unpair all?", "Every device will need to be paired again"))
    Current()->Forget(-1, Report("Unpair all"));
  ImGui::Spacing();
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const int columns = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 60 ? 3 : 2;
  const float w = (ImGui::GetContentRegionAvail().x - gap * (columns - 1)) / columns;
  for (int i = 0; i < d.slotCount; ++i) {
    if (i % columns) ImGui::SameLine();
    SlotCard(d, i, w);
  }
}

void Ui::Radio(const DongleState& d) {
  Title("Radio", lh::ui::Icon::Wireless);
  // every slot, one row each
  if (ImGui::BeginTable("radio", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
    ImGui::TableSetupColumn("Device");
    for (const char* c : {"Polls/s", "Loss", "Resent/s", "Frames/s", "Data", "On air", "Signal d / t"})
      ImGui::TableSetupColumn(c, ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * (c[0] == 'S' ? 7.5f : 5.2f));
    ImGui::TableHeadersRow();
    for (int i = 0; i < d.slotCount; ++i) {
      const auto& s = d.slots[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char label[8];
      std::snprintf(label, sizeof label, "%d", i);
      if (ImGui::Selectable(label, selectedSlot_ == i, ImGuiSelectableFlags_SpanAllColumns)) selectedSlot_ = i;
      ImGui::TableNextColumn();
      if (!s.used) { ImGui::TextDisabled("Empty"); continue; }
      ImGui::TextColored(s.active ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kOff, "%s", DeviceLabel(s).c_str());
      if (!s.active) continue;
      ImGui::TableNextColumn(); ImGui::Text("%.0f", s.pollsPerSec);
      ImGui::TableNextColumn(); ImGui::TextColored(LossColor(s.lossPercent), "%.1f %%", std::max(0.0, s.lossPercent));
      ImGui::TableNextColumn(); ImGui::Text("%.0f", s.resendsPerSec + s.dupPerSec);
      ImGui::TableNextColumn(); ImGui::Text("%.0f", s.framesPerSec);
      ImGui::TableNextColumn(); ImGui::Text("%.1f kB/s", s.kBytesPerSec);
      ImGui::TableNextColumn();
      if (s.airKBytesPerSec > 0) ImGui::Text("%.1f kB/s", s.airKBytesPerSec); else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::TextColored(RssiColor(s.rssiDongle), "%d", s.rssiDongle);
      ImGui::SameLine(0, 4); ImGui::TextDisabled("/"); ImGui::SameLine(0, 4);
      ImGui::TextColored(RssiColor(s.rssiDevice), "%d dBm", s.rssiDevice);
    }
    ImGui::EndTable();
  }
  // the selected slot in detail: graphs over the last two minutes
  const auto& s = d.slots[selectedSlot_];
  ImGui::Spacing();
  char head[160];
  std::snprintf(head, sizeof head, "Slot %d: %s", selectedSlot_, s.used ? DeviceLabel(s).c_str() : "empty");
  ImGui::SeparatorText(head);
  if (!s.active) { ImGui::TextDisabled(s.used ? "Not connected" : "Nothing paired"); return; }
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float gw = (ImGui::GetContentRegionAvail().x - gap * 2) / 3, gh = ImGui::GetFontSize() * 6;
  char o[64];
  std::snprintf(o, sizeof o, "loss %.1f %%", std::max(0.0, s.lossPercent));
  Graph("##loss", s.lossHistory, 0, 25, LossColor(s.lossPercent), o, gw, gh);
  ImGui::SameLine();
  std::snprintf(o, sizeof o, "signal %d dBm", s.rssiDongle);
  Graph("##rssi", s.rssiHistory, -95, -35, RssiColor(s.rssiDongle), o, gw, gh);
  ImGui::SameLine();
  float peak = 4;
  for (float x : s.kbpsHistory) peak = std::max(peak, x * 1.2f);
  std::snprintf(o, sizeof o, "%.1f kB/s", s.kBytesPerSec);
  Graph("##kbps", s.kbpsHistory, 0, peak, kInfo, o, gw, gh);
  ImGui::TextDisabled("last two minutes");
  ImGui::Spacing();
  if (ImGui::BeginTable("detail", 4, ImGuiTableFlags_SizingStretchProp)) {
    const auto row = [](const char* a, const std::string& b, const char* c, const std::string& e) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::TextDisabled("%s", a);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
      ImGui::TableNextColumn(); ImGui::TextDisabled("%s", c);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(e.c_str());
    };
    char a[64], b[64];
    std::snprintf(a, sizeof a, "%.0f Hz", s.imuHz); std::snprintf(b, sizeof b, "%.0f / s", s.lightPerSec);
    row("IMU", a, "Light frames", b);
    std::snprintf(a, sizeof a, "%.1f / s", s.crcPerSec); std::snprintf(b, sizeof b, "%.1f / s", s.timeoutsPerSec);
    row("Corrupted answers", a, "No answer", b);
    std::snprintf(a, sizeof a, "%d", s.deviceMissed); std::snprintf(b, sizeof b, "%d", s.deviceCrc);
    row("Polls the device missed", a, "Corrupted polls (device)", b);
    std::snprintf(a, sizeof a, "%.0f / s", s.dropsPerSec);
    std::snprintf(b, sizeof b, "%d .. %d dBm", s.rssiMin, s.rssiMax);
    row("Dropped on the dongle", a, "Signal range", b);
    if (s.saturationPercent >= 0) {
      std::snprintf(a, sizeof a, "%d %%", s.saturationPercent);
      row("Saturation", a, "", "");
    }
    std::snprintf(a, sizeof a, "%.0f / s", s.deviceDropsPerSec); std::snprintf(b, sizeof b, "%.1f / s", s.deviceReplyDropsPerSec);
    row("Dropped on the device", a, "Replies dropped", b);
    if (s.airKBytesPerSec > 0 && s.kBytesPerSec > 0) {
      std::snprintf(a, sizeof a, "%.0f %% smaller", 100.0 * (1 - s.airKBytesPerSec / s.kBytesPerSec));
      std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(s.frames));
      row("Compression", a, "Frames since start", b);
    }
    ImGui::EndTable();
  }
}

void Ui::Channels(const DongleState& d) {
  Title("Channels", lh::ui::Icon::Channels);
  ImGui::TextWrapped("Loss per channel. Grey ones are skipped for now");
  ImGui::Spacing();
  // hop channels: loss bars
  {
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 9;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(ImVec4(0.06f, 0.07f, 0.09f, 1)), 4);
    const float bw = w / kHops, label = ImGui::GetTextLineHeight() * 2.2f, top = ImGui::GetTextLineHeight();
    for (int k = 0; k < kHops; ++k) {
      const bool on = (d.channelMask >> k) & 1;
      const bool idle = d.joined == 0;   // nothing connected: no traffic, so no loss to measure
      const float loss = idle ? 0.f : d.hopLoss[k];
      const float frac = std::min(1.f, loss / 30.f);
      const float x0 = p.x + k * bw + bw * 0.18f, x1 = p.x + (k + 1) * bw - bw * 0.18f;
      const float base = p.y + h - label;
      const ImVec4 c = !on || idle ? kOff : LossColor(loss);
      dl->AddRectFilled(ImVec2(x0, base - std::max(2.f, frac * (h - label - top))), ImVec2(x1, base), U32(c, on ? 0.9f : 0.35f), 2);
      char t[16];
      std::snprintf(t, sizeof t, "%d", 2400 + d.hopChannel[k]);
      const ImVec2 ts = ImGui::CalcTextSize(t);
      dl->AddText(ImVec2((x0 + x1 - ts.x) / 2, base + 2), U32(on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kOff), t);
      if (idle) std::snprintf(t, sizeof t, on ? "-" : "off");
      else std::snprintf(t, sizeof t, on ? "%d%%" : "off", int(loss));
      const ImVec2 ls = ImGui::CalcTextSize(t);
      dl->AddText(ImVec2((x0 + x1 - ls.x) / 2, base + 2 + ts.y), U32(kOff), t);
      if (d.hopChannel[k] == d.currentChannel && on) dl->AddRect(ImVec2(x0 - 2, p.y + 2), ImVec2(x1 + 2, base), U32(kInfo, 0.6f), 2);
    }
    ImGui::Dummy(ImVec2(w, h));
  }
  int on = 0;
  for (int k = 0; k < kHops; ++k) on += (d.channelMask >> k) & 1;
  if (d.joined == 0) ImGui::TextDisabled("%d of %d channels in use  |  no device connected: loss shows once one is", on, kHops);
  else ImGui::TextDisabled("%d of %d channels in use  |  MHz, loss over the last couple of seconds", on, kHops);
  ImGui::Spacing();
  ImGui::SeparatorText("2.4 GHz band");
  if (BigButton("Scan the band", ImGui::GetFontSize() * 12)) Current()->Scan(Report("Scan"));
  Tip("Devices pause for a moment");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (d.scan.empty()) ImGui::TextDisabled("Not scanned yet");
  else ImGui::TextDisabled("Scanned %.0f s ago. Wi-Fi channels 1, 6 and 11 are marked", Now() - d.scanTime);
  if (!d.scan.empty()) {
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 9;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(ImVec4(0.06f, 0.07f, 0.09f, 1)), 4);
    const int n = int(d.scan.size());
    const float bw = w / n, label = ImGui::GetTextLineHeight() * 1.3f;
    const float base = p.y + h - label;
    // Wi-Fi channels 1/6/11 (22 MHz wide around 2412/2437/2462)
    for (int wifi : {12, 37, 62}) {
      const float x0 = p.x + (wifi - 11) * bw, x1 = p.x + (wifi + 11) * bw;
      dl->AddRectFilled(ImVec2(x0, p.y + 2), ImVec2(x1, base), U32(kAccent, 0.06f));
      char t[16];
      std::snprintf(t, sizeof t, "Wi-Fi %d", wifi == 12 ? 1 : wifi == 37 ? 6 : 11);
      dl->AddText(ImVec2(x0 + 4, p.y + 4), U32(kAccent, 0.6f), t);
    }
    for (int i = 0; i < n; ++i) {
      if (d.scan[i] == 0xFF) continue;
      const float dbm = -float(d.scan[i]);  // -100 (quiet) .. -40 (busy)
      const float frac = std::clamp((dbm + 100.f) / 60.f, 0.f, 1.f);
      const ImVec4 c = dbm > -60 ? kBad : dbm > -75 ? kWarn : kGood;
      dl->AddRectFilled(ImVec2(p.x + i * bw + 1, base - std::max(2.f, frac * (h - label - 4))), ImVec2(p.x + (i + 1) * bw - 1, base), U32(c, 0.85f));
    }
    for (int k = 0; k < kHops; ++k) {   // our hop channels
      const int ch = d.hopChannel[k];
      if (ch >= n) continue;
      const float x = p.x + (ch + 0.5f) * bw;
      dl->AddTriangleFilled(ImVec2(x, base + 2), ImVec2(x - 4, base + 9), ImVec2(x + 4, base + 9),
                            U32((d.channelMask >> k) & 1 ? kInfo : kOff));
    }
    for (int mhz = 0; mhz <= 80; mhz += 10) {
      char t[8];
      std::snprintf(t, sizeof t, "%d", 2400 + mhz);
      dl->AddText(ImVec2(p.x + mhz * bw, base + 10), U32(kOff), t);
    }
    ImGui::Dummy(ImVec2(w, h + ImGui::GetTextLineHeight()));
    ImGui::TextDisabled("Blue: channels in use");
  }
}

// ---- Repeater (firmware that answers cmd 0x3F) ----
// A repeater is a second dongle that listens to the main's devices from elsewhere in the room and hands
// the main the answers it missed. The page is one scene in every state: the dongle strip, the link map
// (the main, its two repeater sockets, the devices between them) and an action row. State is drawn as
// shape and colour; the numbers are in tooltips. Repeater ids 0 and 1 are shown as R1 and R2.

namespace {
using lh::ui::Icon;

constexpr uint32_t kInfoDongles = 1, kInfoMapMain = 2, kInfoMapRep = 4, kInfoPlace = 16;
constexpr double kLossScale = 30;   // loss bars: 0 to 30 %
const ImVec4 kText{0.92f, 0.94f, 0.97f, 1};
const ImVec4 kMuted{0.62f, 0.64f, 0.68f, 1};
const ImVec4 kTrack{0.06f, 0.07f, 0.09f, 1};

std::shared_ptr<Dongle> FindByNetBase(const std::vector<std::shared_ptr<Dongle>>& all, uint32_t base, const std::string& not_serial) {
  for (const auto& o : all)
    if (o->Serial() != not_serial && o->Snapshot().netBase == base && base) return o;
  return nullptr;
}
std::shared_ptr<Dongle> FindBySerial(const std::vector<std::shared_ptr<Dongle>>& all, const std::string& serial) {
  for (const auto& o : all)
    if (o->Serial() == serial) return o;
  return nullptr;
}
std::string Short4(const std::string& s) { return ShortSerial(s).substr(0, 4); }


// The repeater ids a main has: in its mask, a repeater of it plugged in here, or one that answered in the last
// 10 minutes.
uint8_t KnownIds(const std::vector<std::shared_ptr<Dongle>>& all, const DongleState& m) {
  uint8_t ids = m.satMask;
  for (int k = 0; k < kMaxRepeaters; ++k)
    if (m.relaySats[k].msSinceSrep < 600000 && !(m.autoSeen >> k & 1)) ids |= uint8_t(1u << k);   // (one that joined on its own just goes)
  for (const auto& o : all) {
    const auto s = o->Snapshot();
    if (o->Serial() != m.serial && s.haveRole && s.role == 1 && s.mainBase == m.netBase) ids |= uint8_t(1u << s.satId);
  }
  return ids;
}
// Most likely the spare and the main swapped: the dongle with the devices would become the repeater.
bool WrongWay(const DongleState& from, const DongleState& m) { return from.paired > 0 && m.paired == 0; }

// A repeater's own state, on USB.
enum class Sat { Starting, Searching, Following, OffAtMain };
Sat SatState(const DongleState& d, const DongleState* main) {
  if (d.roleRunning != 1) return Sat::Starting;
  if (!d.satSynced) return Sat::Searching;
  if (main && !(main->satMask & (1u << d.satId))) return Sat::OffAtMain;
  return Sat::Following;
}
ImVec4 SatColor(Sat s) { return s == Sat::Following ? kGood : s == Sat::Searching ? kWarn : s == Sat::OffAtMain ? kInfo : kOff; }
const char* SatWord(Sat s) {
  return s == Sat::Following ? "FOLLOWING" : s == Sat::Searching ? "SEARCHING" : s == Sat::OffAtMain ? "OFF AT MAIN" : "STARTING";
}

// ---- drawing primitives (sizes follow the font, like the rest of the app) ----

float Px() { return lh::ui::Scale(); }
float TitleHeight() {
  ImGui::PushFont(g_title);
  const float h = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  return h;
}
ImVec2 TextSize(const char* t, ImFont* font = nullptr) {
  if (font) ImGui::PushFont(font);
  const ImVec2 s = ImGui::CalcTextSize(t);
  if (font) ImGui::PopFont();
  return s;
}
void TextAt(ImDrawList* dl, ImVec2 p, const ImVec4& c, const char* t, ImFont* font = nullptr, float alpha = 1) {
  if (font) ImGui::PushFont(font);
  dl->AddText(p, U32(c, alpha), t);
  if (font) ImGui::PopFont();
}
void TextCentered(ImDrawList* dl, ImVec2 c, const ImVec4& col, const char* t, ImFont* font = nullptr, float alpha = 1) {
  const ImVec2 s = TextSize(t, font);
  TextAt(dl, ImVec2(c.x - s.x / 2, c.y - s.y / 2), col, t, font, alpha);
}

// A tooltip for a drawn area (no item there).
void AreaTip(ImVec2 a, ImVec2 b, const std::string& text) {
  if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseHoveringRect(a, b)) ImGui::SetTooltip("%s", text.c_str());
}

// Signal bars centred on c, h high, on a dark pill so they read over a line.
void SignalPill(ImDrawList* dl, ImVec2 c, int rssi, float h) {
  const float total = h * (4 * 0.22f + 3 * 0.12f), padX = h * 0.3f, padY = h * 0.18f;
  const ImVec2 p(c.x - total / 2, c.y - h / 2);
  dl->AddRectFilled(ImVec2(p.x - padX, p.y - padY), ImVec2(p.x + total + padX, p.y + h + padY), U32(ImVec4(0.067f, 0.075f, 0.098f, 1)), h * 0.3f);
  DrawBars(dl, p, h, rssi);
}

enum class Stroke { Solid, Dashed, Dotted, Broken };
ImVec2 Lerp(ImVec2 a, ImVec2 b, float t) { return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }
ImVec2 Bez(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, float t) {
  const float u = 1 - t;
  return ImVec2(u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y);
}
// A link: a cubic curve (a line when the control points lie on it) in one of four styles.
void Curve(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 col, float th, Stroke st) {
  constexpr int n = 40;
  ImVec2 pts[n + 1];
  float len[n + 1];
  len[0] = 0;
  for (int i = 0; i <= n; ++i) {
    pts[i] = Bez(p0, p1, p2, p3, float(i) / n);
    if (i) len[i] = len[i - 1] + std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
  }
  if (st == Stroke::Solid) { dl->AddPolyline(pts, n + 1, col, ImDrawFlags_None, th); return; }
  if (st == Stroke::Broken) {   // a gap in the middle, its ends marked
    constexpr int g0 = n * 2 / 5, g1 = n * 3 / 5;
    dl->AddPolyline(pts, g0 + 1, col, ImDrawFlags_None, th);
    dl->AddPolyline(pts + g1, n - g1 + 1, col, ImDrawFlags_None, th);
    const float s = th + 4 * Px();
    for (int e : {g0, g1}) dl->AddLine(ImVec2(pts[e].x - s * 0.4f, pts[e].y + s), ImVec2(pts[e].x + s * 0.4f, pts[e].y - s), col, th);
    return;
  }
  const auto at = [&](float dist) {
    int i = 1;
    while (i < n && len[i] < dist) ++i;
    const float seg = len[i] - len[i - 1];
    return Lerp(pts[i - 1], pts[i], seg > 0 ? (dist - len[i - 1]) / seg : 0);
  };
  const float px = Px();
  if (st == Stroke::Dotted) {
    for (float d = 0; d <= len[n]; d += 5 * px) dl->AddCircleFilled(at(d), std::max(th * 0.55f, 0.9f * px), col, 8);
    return;
  }
  for (float d = 0; d < len[n]; d += 11 * px) dl->AddLine(at(d), at(std::min(len[n], d + 6 * px)), col, th);
}
void Line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th, Stroke st) {
  Curve(dl, a, Lerp(a, b, 1.f / 3), Lerp(a, b, 2.f / 3), b, col, th, st);
}
void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th) {
  a = ImVec2(a.x + th, a.y + th);   // inside the box, like AddRect
  b = ImVec2(b.x - th, b.y - th);
  Line(dl, a, ImVec2(b.x, a.y), col, th, Stroke::Dashed);
  Line(dl, ImVec2(b.x, a.y), b, col, th, Stroke::Dashed);
  Line(dl, b, ImVec2(a.x, b.y), col, th, Stroke::Dashed);
  Line(dl, ImVec2(a.x, b.y), a, col, th, Stroke::Dashed);
}
// A radio link from a to b (level): coloured by its signal and 1 to 4 px thick (its bar count), its bars on a
// pill at pillX, and the dBm as a tooltip over the link, halfH above and below.
void SignalLink(ImDrawList* dl, ImVec2 a, ImVec2 b, int rssi, float pillX, float pillH, float halfH) {
  dl->AddLine(a, b, U32(RssiColor(rssi), 0.85f), float(std::max(1, Bars(rssi))) * Px());
  SignalPill(dl, ImVec2(pillX, a.y), rssi, pillH);
  char t[24];
  std::snprintf(t, sizeof t, "%d dBm", rssi);
  AreaTip(ImVec2(a.x, a.y - halfH), ImVec2(b.x, b.y + halfH), t);
}

// A dot travelling along a link while frames go over it.
void Pulse(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, const ImVec4& col, double now, double speed = 0.8) {
  const float t = float(std::fmod(now * speed, 1.0));
  const ImVec2 c = Bez(p0, p1, p2, p3, t);
  dl->AddCircleFilled(c, 4.5f * Px(), U32(col, 0.25f), 12);
  dl->AddCircleFilled(c, 2.5f * Px(), U32(col), 12);
}
void PulseLine(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& col, double now, double speed = 0.8) {
  Pulse(dl, a, Lerp(a, b, 1.f / 3), Lerp(a, b, 2.f / 3), b, col, now, speed);
}

// A card: the page's card colour with an edge.
void Card(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& edge, float edgeAlpha = 0.55f, float th = 1) {
  const float r = ImGui::GetFontSize() * 0.5f;
  dl->AddRectFilled(a, b, U32(lh::ui::CardColor()), r);
  dl->AddRect(a, b, U32(edge, edgeAlpha), r, 0, th * Px());
}

// One pip per slot the dongle has: filled green = connected, filled grey = paired and off, hollow = empty.
int PipCount(const DongleState& d) { return std::clamp(d.slotCount, 1, kSlots); }
float SlotPips(ImDrawList* dl, ImVec2 left, float r, const DongleState& d, float alpha = 1) {
  for (int i = 0; i < PipCount(d); ++i) {
    const ImVec2 c(left.x + r + float(i) * r * 2.7f, left.y);
    const auto& s = d.slots[i];
    if (s.used) dl->AddCircleFilled(c, r, U32(s.active ? kGood : kOff, alpha), 12);
    else dl->AddCircle(c, r - 0.5f * Px(), U32(kOff, 0.6f * alpha), 12, Px());
  }
  return r * 2.7f * float(PipCount(d));
}

// A dongle's node, its parts stacked in the middle of [a, b]: icon, role word, serial, then the slot pips (pips)
// or a line of text (extra). absent: the dongle isn't plugged in (a dashed outline, that text for the serial).
void NodeCard(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& c, const char* word, const std::string& id,
              const DongleState* pips = nullptr, const char* extra = nullptr, const char* absent = nullptr) {
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), tlh = TitleHeight(), iconS = fs * 2.2f;
  const float cx = (a.x + b.x) / 2;
  const bool more = !absent && (pips || extra);
  const ImVec4 col = absent ? kOff : c;
  if (absent) DashedRect(dl, a, b, U32(kOff, 0.6f), Px());
  else Card(dl, a, b, c, 0.75f, 1.5f);
  float y = (a.y + b.y - iconS - fs * 0.3f - tlh - lh - (more ? fs * 0.6f + lh : 0)) / 2;
  lh::ui::DrawIcon(dl, Icon::Dongle, ImVec2(cx, y + iconS / 2), iconS, U32(col, absent ? 0.6f : 1));
  y += iconS + fs * 0.3f;
  TextCentered(dl, ImVec2(cx, y + tlh / 2), col, word, g_title);
  y += tlh;
  TextCentered(dl, ImVec2(cx, y + lh / 2), kMuted, absent ? absent : id.c_str());
  y += lh + fs * 0.6f + lh / 2;
  if (!more) return;
  const float r = fs * 0.22f;
  if (pips) SlotPips(dl, ImVec2(cx - r * 2.7f * float(PipCount(*pips)) / 2, y), r, *pips);
  else TextCentered(dl, ImVec2(cx, y), kText, extra);
}

// Three pips: how much of what was asked came back.
void HealthPips(ImDrawList* dl, ImVec2 left, float r, int n, const ImVec4& col, bool hollow) {
  for (int i = 0; i < 3; ++i) {
    const ImVec2 c(left.x + r + float(i) * r * 2.7f, left.y);
    if (!hollow && i < n) dl->AddCircleFilled(c, r, U32(col), 12);
    else dl->AddCircle(c, r - 0.5f * Px(), U32(col, hollow ? 0.8f : 0.35f), 12, Px());
  }
}
int AnswerPips(const DongleState::RelaySat& r) {
  if (r.asksPerSec < 0.5) return 3;
  const double f = r.srepsPerSec / r.asksPerSec;
  return f > 0.9 ? 3 : f > 0.5 ? 2 : 1;
}

// Diagonal lines over a rect, `step` apart.
void Hatch(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float step, float th) {
  const float h = b.y - a.y;
  dl->PushClipRect(a, b, true);
  for (float x = a.x - h; x < b.x; x += step) dl->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), col, th);
  dl->PopClipRect();
}
// The "saved" fill: green stripes.
void SavedFill(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImDrawFlags corners) {
  dl->AddRectFilled(a, b, U32(kGood, 0.28f), r, corners);
  Hatch(dl, a, b, U32(kGood, 0.85f), 4 * Px(), 1.5f * Px());
}

// Before and after on a 0 to 30 % scale: the whole coloured length is what the main misses alone; the
// striped green part is what the repeater saved; the solid part (coloured by loss) is still lost.
void LossBar(ImDrawList* dl, ImVec2 a, ImVec2 b, double own, double merged) {
  const float w = b.x - a.x, r = (b.y - a.y) * 0.3f, px = Px();
  dl->AddRectFilled(a, b, U32(kTrack), r);
  for (double t : {10.0, 20.0}) {   // scale ticks
    const float x = a.x + w * float(t / kLossScale);
    dl->AddLine(ImVec2(x, a.y + 1), ImVec2(x, b.y - 1), U32(kOff, 0.18f), px);
  }
  if (own < 0) return;
  merged = std::clamp(merged, 0.0, own);
  const auto X = [&](double v) { return a.x + w * float(std::clamp(v / kLossScale, 0.0, 1.0)); };
  const float xm = X(merged), xo = X(own);
  if (xo - xm > 0.5f) SavedFill(dl, ImVec2(xm, a.y), ImVec2(xo, b.y), r, ImDrawFlags_RoundCornersRight);
  if (merged > 0) dl->AddRectFilled(a, ImVec2(std::max(xm, a.x + 2 * px), b.y), U32(LossColor(merged), 0.95f), r, ImDrawFlags_RoundCornersLeft);
  if (own > kLossScale) {   // off the scale: a notch at the end
    const float h = b.y - a.y;
    dl->AddTriangleFilled(ImVec2(b.x - h * 0.6f, a.y), ImVec2(b.x, a.y + h / 2), ImVec2(b.x - h * 0.6f, b.y), U32(kText, 0.9f));
  }
}

// 0 to 100 %, one segment: green above 90 %, yellow above 60 %, red below.
void HeardBar(ImDrawList* dl, ImVec2 a, ImVec2 b, double frac) {
  const float r = (b.y - a.y) * 0.3f;
  dl->AddRectFilled(a, b, U32(kTrack), r);
  if (frac <= 0) return;
  const ImVec4 c = frac > 0.9 ? kGood : frac > 0.6 ? kWarn : kBad;
  dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * float(std::min(1.0, frac)), b.y), U32(c, 0.9f), r);
}

// A dongle's LED patterns (doorman/dongle-fw/src/main.c): a repeater following the main is on with a short
// gap each second; a repeater searching blinks at 4 Hz; a normal dongle (not a repeater) blips: 20 ms of
// every 200 ms with no PC, 100 ms of every second with one.
enum class Led { Following, Searching, Normal };
// The LED as it blinks now: a live dot, then one period of the pattern as a row of blocks with the moment
// now lit.
float LedGlyph(ImDrawList* dl, ImVec2 left, float h, Led led, double now, float alpha = 1) {
  const ImVec4 c = led == Led::Following ? kGood : led == Led::Searching ? kWarn : kOff;
  // Normal: the dot stays lit 40 ms (two frames), so a 20 ms blip shows on screen
  const bool lit = led == Led::Following ? std::fmod(now, 1.0) > 0.1 : led == Led::Searching ? std::fmod(now, 0.25) < 0.125 : std::fmod(now, 0.2) < 0.04;
  const float r = h * 0.3f;
  const ImVec2 dot(left.x + r, left.y);
  if (lit) {
    dl->AddCircleFilled(dot, r * 1.7f, U32(c, 0.22f * alpha), 16);
    dl->AddCircleFilled(dot, r, U32(c, alpha), 16);
  } else {
    dl->AddCircle(dot, r, U32(c, 0.5f * alpha), 16, Px());
  }
  const int n = led == Led::Searching ? 8 : 10;
  const double step = led == Led::Following ? 0.1 : led == Led::Searching ? 0.0625 : 0.02;
  const int cur = int(std::fmod(now, step * n) / step);
  const float bw = h * 0.24f, gap = h * 0.08f, x0 = left.x + r * 2 + h * 0.35f;
  for (int i = 0; i < n; ++i) {
    const bool on = led == Led::Following ? i != 0 : led == Led::Searching ? (i % 4) < 2 : i == 0;
    const ImVec2 a(x0 + float(i) * (bw + gap), left.y - h * 0.28f), b(a.x + bw, left.y + h * 0.28f);
    if (on) dl->AddRectFilled(a, b, U32(c, (i == cur ? 1.f : 0.45f) * alpha), 1);
    else dl->AddRect(a, b, U32(c, (i == cur ? 0.8f : 0.3f) * alpha), 1, 0, Px());
  }
  return x0 + float(n) * (bw + gap) - left.x;
}

// Rings travelling out from the repeater toward the main while it searches.
void Radar(ImDrawList* dl, ImVec2 c, float radius, const ImVec4& col, double now) {
  const float pi = 3.14159265f;
  for (int i = 0; i < 3; ++i) {
    const float t = float(std::fmod(now + i * 0.5, 1.5) / 1.5);
    lh::ui::Arc(dl, c, radius * (0.2f + 0.8f * t), pi * 0.8f, pi * 1.2f, U32(col, (1 - t) * 0.8f), 1.5f * Px());
  }
}

// Three dots lighting in turn: waiting for something.
void WaitDots(ImDrawList* dl, ImVec2 left, float r, const ImVec4& col, double now) {
  for (int i = 0; i < 3; ++i) {
    const float a = 0.25f + 0.75f * float(std::max(0.0, std::sin((now * 3 - i * 0.6) * 1.6)));
    dl->AddCircleFilled(ImVec2(left.x + r + float(i) * r * 3, left.y), r, U32(col, a), 12);
  }
}

// A row of its own that only waits (a read hasn't come yet).
void WaitRow(double now) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  WaitDots(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + ImGui::GetTextLineHeight() / 2), ImGui::GetFontSize() * 0.3f, kOff, now);
  ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 3, ImGui::GetTextLineHeight()));
}

// The Update chip (firmware too old), its left edge at x, centred on cy.
void UpdateChip(ImDrawList* dl, float x, float cy) {
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight();
  const ImVec2 ca(x, cy - lh / 2 - fs * 0.2f), cb(x + ImGui::CalcTextSize("Update").x + fs, cy + lh / 2 + fs * 0.2f);
  dl->AddRectFilled(ca, cb, U32(kWarn, 0.18f), fs * 0.3f);
  dl->AddRect(ca, cb, U32(kWarn, 0.8f), fs * 0.3f, 0, Px());
  TextAt(dl, ImVec2(ca.x + fs * 0.5f, cy - lh / 2), kWarn, "Update");
}

// "This dongle firmware has no ..." and the button to the Firmware page: true when clicked.
bool NeedsFirmware(const char* what) {
  ImGui::Spacing();
  ImGui::AlignTextToFramePadding();
  Dot(kWarn, 5, true);
  ImGui::TextColored(kWarn, "This dongle firmware has no %s.", what);
  ImGui::SameLine();
  return TintButton("Open firmware page", kAccent);
}

// An arrowhead at tip, pointing along (dx, dy).
void Arrowhead(ImDrawList* dl, ImVec2 tip, float dx, float dy, float size, ImU32 col) {
  const float l = std::hypot(dx, dy);
  if (l <= 0) return;
  dx /= l; dy /= l;
  dl->AddTriangleFilled(tip, ImVec2(tip.x - dx * size - dy * size * 0.6f, tip.y - dy * size + dx * size * 0.6f),
                        ImVec2(tip.x - dx * size + dy * size * 0.6f, tip.y - dy * size - dx * size * 0.6f), col);
}

// The A/B switch: a slider with I (on) and O (off). Returns true when clicked.
bool PowerSwitch(const char* id, bool on, const ImVec4& c) {
  const float fh = ImGui::GetFrameHeight(), h = fh * 0.78f, w = h * 2.1f, px = Px();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, fh));
  const bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a(p.x, p.y + (fh - h) / 2), b(p.x + w, a.y + h);
  const float k = held ? 0.75f : hover ? 0.6f : 0.45f;
  dl->AddRectFilled(a, b, on ? U32(ImVec4(c.x * k, c.y * k, c.z * k, 1)) : U32(ImVec4(0.14f, 0.16f, 0.20f, 1)), h / 2);
  dl->AddRect(a, b, U32(on ? c : kOff, hover ? 0.9f : 0.6f), h / 2, 0, px);
  const float ks = h - 4 * px;
  const ImVec2 ka(on ? b.x - 2 * px - ks : a.x + 2 * px, a.y + 2 * px);
  dl->AddRectFilled(ka, ImVec2(ka.x + ks, ka.y + ks), U32(on ? kText : kOff), ks / 2);
  // I on the free side when on, O when off
  const float fx = on ? (a.x + ka.x) / 2 : (ka.x + ks + b.x) / 2, cy = (a.y + b.y) / 2;
  if (on) dl->AddLine(ImVec2(fx, cy - h * 0.22f), ImVec2(fx, cy + h * 0.22f), U32(kText), 1.5f * px);
  else dl->AddCircle(ImVec2(fx, cy), h * 0.2f, U32(kOff), 12, 1.5f * px);
  return clicked;
}

// A small tinted button that fits a text row.
bool SmallTint(const char* label, const ImVec4& c) {
  PushTint(c);
  const bool r = ImGui::Button(label);
  ImGui::PopStyleColor(3);
  return r;
}

// ---- info blocks: a (?) at a section header's right edge opens a card under it ----

struct InfoLine { ImVec4 color; const char* lead; const char* text; };
using Info = std::initializer_list<InfoLine>;

void InfoText(Info lines) {
  for (const auto& l : lines) {
    if (!l.lead && !l.text) { ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.35f)); continue; }
    if (l.lead) {
      ImGui::TextColored(l.color, "%s", l.lead);
      if (l.text) ImGui::SameLine();
    }
    if (l.text) ImGui::TextWrapped("%s", l.text);
  }
}

// The (?) button: Icon::About in a round button. Returns true when clicked.
bool InfoButton(const char* id, bool open) {
  const float s = ImGui::GetTextLineHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
  const bool hover = ImGui::IsItemHovered();
  if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImU32 col = open ? U32(kInfo) : hover ? IM_COL32(205, 212, 226, 255) : lh::ui::kMutedText;
  lh::ui::DrawIcon(ImGui::GetWindowDrawList(), Icon::About, ImVec2(p.x + s / 2, p.y + s / 2), s, col);
  return clicked;
}

// A (?) that shows its text while hovered (for small things inside the map).
void InfoHover(const char* id, Info lines) {
  InfoButton(id, false);
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
    InfoText(lines);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Extras for a section: header() draws widgets in a strip `width` wide left of the (?) (at: the strip's top
// left, one text line high); body() draws under the info text, inside the info block.
struct SectionExtra {
  float width = 0;
  std::function<void(ImVec2 at)> header;
  std::function<void()> body;
};

// A section header: the label in capitals, a rule, (?) at the right (and an x to close when close is
// given); the info block opens under it.
void Section(const char* label, uint32_t& open, uint32_t bit, Info lines, bool* close = nullptr, const SectionExtra* extra = nullptr) {
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lh = ImGui::GetTextLineHeight(), w = ImGui::GetContentRegionAvail().x, fs = ImGui::GetFontSize();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float tw = ImGui::CalcTextSize(label).x;
  dl->AddText(p, U32(kMuted), label);
  const float right = p.x + w - lh - (close ? lh + fs * 0.6f : 0);
  const float strip = extra && extra->width > 0 ? extra->width + fs * 0.6f : 0;
  dl->AddLine(ImVec2(p.x + tw + fs * 0.6f, p.y + lh / 2), ImVec2(std::max(p.x + tw + fs * 0.6f, right - fs * 0.6f - strip), p.y + lh / 2),
              ImGui::GetColorU32(ImGuiCol_Separator), Px());
  ImGui::PushID(label);
  if (strip > 0 && extra->header) extra->header(ImVec2(right - strip, p.y));
  ImGui::SetCursorScreenPos(ImVec2(right, p.y));
  if (InfoButton("info", open & bit)) open ^= bit;
  if (close) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - lh, p.y));
    const ImVec2 c(p.x + w - lh / 2, p.y + lh / 2);
    *close = ImGui::InvisibleButton("close", ImVec2(lh, lh));
    const bool hover = ImGui::IsItemHovered();
    const float s = lh * 0.3f;
    const ImU32 col = hover ? IM_COL32(235, 240, 248, 255) : lh::ui::kMutedText;
    dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 1.5f * Px());
    dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, 1.5f * Px());
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(w, lh));
  if (!(open & bit)) return;
  // the block: a card with a blue bar on its left edge
  ImGui::PushStyleColor(ImGuiCol_ChildBg, lh::ui::CardColor());
  ImGui::BeginChild((std::string("##info") + label).c_str(), ImVec2(w, 0),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PushTextWrapPos(std::min(w, fs * 60) - ImGui::GetStyle().WindowPadding.x);
  InfoText(lines);
  if (extra && extra->body) extra->body();
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  dl->AddRectFilled(a, ImVec2(a.x + 3 * Px(), b.y), U32(kInfo), Px());
}

const InfoLine kNone{kText, nullptr, nullptr};

// Two dongles and an arrow between them: "this becomes a repeater of that", for the confirms.
void PairPicture(const ImVec4& lc, const char* lw, const std::string& lid, const ImVec4& rc, const char* rw, const std::string& rid,
                 const ImVec4& arrow) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), px = Px();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float bw = fs * 9, bh = lh * 2 + fs * 1.2f, gap = fs * 6;
  const auto box = [&](float x, const ImVec4& c, const char* word, const std::string& id) {
    const ImVec2 a(x, p.y), b(x + bw, p.y + bh);
    Card(dl, a, b, c, 0.7f);
    lh::ui::DrawIcon(dl, Icon::Dongle, ImVec2(a.x + fs * 1.3f, (a.y + b.y) / 2), lh * 1.5f, U32(c));
    TextAt(dl, ImVec2(a.x + fs * 2.4f, a.y + fs * 0.6f), c, word);
    TextAt(dl, ImVec2(a.x + fs * 2.4f, a.y + fs * 0.6f + lh), kText, id.c_str());
  };
  box(p.x, lc, lw, lid);
  box(p.x + bw + gap, rc, rw, rid);
  const float y = p.y + bh / 2, x0 = p.x + bw + fs * 0.6f, x1 = p.x + bw + gap - fs * 0.6f;
  dl->AddLine(ImVec2(x0, y), ImVec2(x1 - fs * 0.5f, y), U32(arrow), 3 * px);
  Arrowhead(dl, ImVec2(x1, y), 1, 0, fs * 0.7f, U32(arrow));
  ImGui::Dummy(ImVec2(bw * 2 + gap, bh));
}

// The info texts.
const Info kInfoDonglesText = {
    {kInfo, "Dongle", "your devices connect to it."},
    {kAccent, "Repeater", "helps a main from another spot in the room."},
    {kText, nullptr, "A dongle with nothing connected becomes a repeater on its own near a dongle with devices."},
    {kText, nullptr, "Click a card to select that dongle."}};
const Info kInfoMapMainText = {
    {kText, nullptr, "A repeater is a second dongle placed elsewhere in the room. The repeater listens to your devices. "
                     "When the main misses an answer, the repeater hands that answer over."},
    kNone,
    {kText, nullptr, "Line colour and thickness show signal. Green is strong, red is weak."},
    kNone,
    {kText, nullptr, "The bar shows loss. The whole bar is what the main misses alone. The striped green part is what the "
                     "repeater saved. The solid part is still lost."},
    kNone,
    {kText, nullptr, "While relaying, a repeater costs about 5 % of the update rate. On standby it costs nothing."}};
const Info kInfoSwitchText = {
    {kGood, "ON", "the main asks this repeater for the answers the main missed."},
    {kOff, "OFF", "the main works alone. The repeater stays linked."},
    {kText, nullptr, "Flip the switch to compare. No device drops out."},
    kNone,
    {kGood, "LIVE", "answering."},
    {kInfo, "STANDBY", "nothing was missed, so the main asked nothing."},
    {kWarn, "WEAK", "answers are late."},
    {kBad, "NO ANSWER", "the repeater is out of range or not powered."}};
const Info kInfoMapRepText = {
    {kText, nullptr, "This map shows what the repeater hears on USB. On a charger the repeater works the same."},
    kNone,
    {kGood, "FOLLOWING", "the repeater hears the main."},
    {kWarn, "SEARCHING", "the main is off or too far."},
    {kInfo, "OFF AT MAIN", "the repeater listens, but the main's switch for this repeater is off."},
    kNone,
    {kText, "heard", "is the share of each device's answers that reach the repeater."}};
const Info kInfoPlaceText = {
    {kText, nullptr, "Unplug the repeater."},
    {kText, nullptr, "Power the repeater from a wall charger. Power banks can switch off."},
    {kText, nullptr, "Put the repeater across the room from the main, at head height, in the open. Pick the side where "
                     "tracking is worst."},
    kNone,
    {kGood, "LED on with a short gap each second:", "following the main."},
    {kWarn, "LED blinking fast:", "searching. Move the repeater closer, or check that the main is on."},
    {kOff, "LED short blips:", "not a repeater. Set up the dongle as a repeater on USB first."},
    {kText, nullptr, "The LED only shows that the repeater hears the main. The main's Repeater page shows whether the main "
                     "uses the repeater."},
    kNone,
    {kText, nullptr, "Then watch the bars here. Flip the switch to compare."}};
const Info kInfoReturnText = {
    {kText, nullptr, "The repeater becomes a normal dongle again and keeps the old pairings."},
    {kText, nullptr, "The main stops asking this repeater."}};
}  // namespace

// ---- state kept across pages ----

// The A/B and mode means, and the set-up's last step (the repeater left USB and answers).
void Ui::PollRepeaters(const std::vector<DongleState>& all, double now) {
  if (now - abLast_ >= 1.0) {   // the A/B: mean loss per device with a repeater on, and with none on
    abLast_ = now;
    for (const auto& m : all) {   // the Timing page's scoreboard: mean loss per device per mode in effect
      if (!m.haveTiming || m.modeNow < 0 || m.modeNow > 3 || (m.haveRole && m.role == 1)) continue;
      auto& mm = modeMeans_[m.serial];
      for (int i = 0; i < kSlots; ++i)
        if (m.slots[i].active && m.slots[i].lossPercent >= 0) { mm.sum[m.modeNow][i] += m.slots[i].lossPercent; mm.n[m.modeNow][i]++; }
    }
    for (const auto& m : all) {
      if (!m.haveRole || m.role != 0 || !m.extras) continue;
      auto& ab = abMeans_[m.serial];
      const int b = m.satMask ? 0 : 1;
      for (int i = 0; i < kSlots; ++i)
        if (m.slots[i].active && m.slots[i].lossPercent >= 0) { ab.sum[b][i] += m.slots[i].lossPercent; ab.n[b][i]++; }
    }
  }
  std::string made, mainSerial;
  int id = 0;
  bool fresh = false;
  {
    std::lock_guard lock(resultsMutex_);
    made = repeaterMade_; mainSerial = repeaterMadeMain_; id = repeaterMadeId_; fresh = madeFresh_;
    madeFresh_ = false;
  }
  if (fresh) { placementOpen_ = true; placeDoneAt_ = 0; }
  if (made.empty()) return;
  const bool onUsb = std::any_of(all.begin(), all.end(), [&](const DongleState& s) { return s.serial == made; });
  for (const auto& m : all)
    if (m.serial == mainSerial && !placeDoneAt_ && !onUsb &&
        SocketState(m.relaySats[id], m.satMask & (1u << id), true) == Sock::Live) {
      placeDoneAt_ = now;
      Toast(RName(id) + " live", false, 3);
    }
  if (placeDoneAt_ && now - placeDoneAt_ > 10) {   // done: the tracker goes
    std::lock_guard lock(resultsMutex_);
    repeaterMade_.clear();
    placeDoneAt_ = 0;
  }
}

// The switch mask shown: the one just set until the dongle reports it (its reads are once a second).
uint8_t Ui::ShownMask(const DongleState& d) {
  const auto it = maskPending_.find(d.serial);
  if (it == maskPending_.end()) return d.satMask;
  if (now_ < it->second.second && d.satMask != it->second.first) return it->second.first;
  maskPending_.erase(it);
  return d.satMask;
}

bool Ui::SetUpFor(const std::string& serial) {
  std::lock_guard lock(resultsMutex_);
  return !repeaterMade_.empty() && (repeaterMade_ == serial || repeaterMadeMain_ == serial);
}

// ---- actions: the dongle's role, then the main's mask ----

void Ui::Link(const std::string& from, const std::string& main, int id) {   // opens the confirm
  linkFrom_ = from; linkTo_ = main; linkId_ = id; openLink_ = true;
}

void Ui::StartLink(const std::string& fromSerial, const std::string& mainSerial, int id) {
  const auto& all = manager_.Dongles();
  const auto from = FindBySerial(all, fromSerial), main = FindBySerial(all, mainSerial);
  if (!from || !main) { Toast("Link failed: dongle unplugged", true, 3); return; }
  const DongleState f = from->Snapshot(), m = main->Snapshot();
  if (id < 0 || KnownIds(all, m) & (1u << id) || WrongWay(f, m)) return;
  const uint32_t base = m.netBase;
  const uint8_t mask = uint8_t(m.satMask | 1u << id);
  from->SetRole(1, id, base, [this, main, mask, fromSerial, mainSerial, id](bool ok, const std::string& msg) {
    if (!ok) { Post("Link failed: " + msg, false); return; }
    main->SetRepeaters(mask, true, [this, fromSerial, mainSerial, id](bool ok2, const std::string& msg2) {
      if (!ok2) { Post("Link failed: " + msg2, false); return; }
      std::lock_guard lock(resultsMutex_);
      results_.push_back({RName(id) + " linked", true, 3});
      repeaterMade_ = fromSerial;
      repeaterMadeMain_ = mainSerial;
      repeaterMadeId_ = id;
      madeFresh_ = true;
    });
  });
}

void Ui::StartReturn(const std::string& serial) {
  const auto& all = manager_.Dongles();
  const auto rep = FindBySerial(all, serial);
  if (!rep) return;
  const DongleState d = rep->Snapshot();
  const auto main = FindByNetBase(all, d.mainBase, d.serial);
  const int id = d.satId;
  rep->SetRole(0, 0, 0, [this, main, id, serial](bool ok, const std::string& msg) {
    if (!ok) { Post("Return failed: " + msg, false); return; }
    {
      std::lock_guard lock(resultsMutex_);
      results_.push_back({"Back to normal", true, 3});
      if (repeaterMade_ == serial) repeaterMade_.clear();
    }
    if (main) {
      const auto m = main->Snapshot();
      if (m.satMask & (1u << id))
        main->SetRepeaters(uint8_t(m.satMask & ~(1u << id)), true, [this](bool ok2, const std::string& msg2) {
          if (!ok2) Post("Switch failed: " + msg2, false);
        });
    }
  });
}

// ---- the page ----

void Ui::Repeater(const DongleState& d) {
  Title("Repeater", Icon::Repeater);
  RepeaterModals();
  DongleStrip(d);
  if (!d.extras) {
    if (NeedsFirmware("repeater role")) page_ = Page::Firmware;
    return;
  }
  if (!d.haveRole) {
    ImGui::Spacing();
    WaitRow(now_);
    return;
  }
  switch (RoleOf(d)) {
    case Role::Repeater: RepeaterSelf(d); break;
    case Role::Spare: RepeaterSpare(d); break;
    case Role::Main: RepeaterMain(d); break;
  }
}

// The two confirms: a picture first, one line, the verb on the button.
void Ui::RepeaterModals() {
  const auto& all = manager_.Dongles();
  if (openLink_) { ImGui::OpenPopup("Link as repeater"); openLink_ = false; }
  if (openReturn_) { ImGui::OpenPopup("Return to normal##confirm"); openReturn_ = false; }
  {
    const auto from = FindBySerial(all, linkFrom_), main = FindBySerial(all, linkTo_);
    const DongleState f = from ? from->Snapshot() : DongleState{}, m = main ? main->Snapshot() : DongleState{};
    const bool can = from && main && linkId_ >= 0 && !(KnownIds(all, m) & (1u << linkId_)) && !WrongWay(f, m);
    const auto body = [&] {
      if (!from || !main) { ImGui::TextColored(kWarn, "Dongle unplugged."); return; }
      PairPicture(RoleColor(f), RoleWord(RoleOf(f)), Short4(f.serial), kInfo, "MAIN", Short4(m.serial), kAccent);
      ImGui::Spacing();
      if (can) ImGui::Text("%s becomes %s of %s.", Short4(f.serial).c_str(), RName(linkId_).c_str(), Short4(m.serial).c_str());
      else ImGui::TextColored(kWarn, "Main is full");
      if (f.paired)
        ImGui::TextColored(kBad, "Its %d paired device%s stop working with it.", f.paired, f.paired == 1 ? "" : "s");
    };
    if (Confirm("Link as repeater", nullptr, "Link", body, can)) StartLink(linkFrom_, linkTo_, linkId_);
  }
  {
    const auto rep = FindBySerial(all, returnFor_);
    const DongleState r = rep ? rep->Snapshot() : DongleState{};
    const bool mainHere = rep && FindByNetBase(all, r.mainBase, r.serial);
    const auto body = [&] {
      if (!rep) { ImGui::TextColored(kWarn, "Dongle unplugged."); return; }
      PairPicture(kAccent, ("REPEATER " + RName(r.satId)).c_str(), Short4(r.serial), kOff, "SPARE", Short4(r.serial), kBad);
      ImGui::Spacing();
      ImGui::Text("%s runs on its own again.", Short4(r.serial).c_str());
      if (!mainHere) ImGui::TextColored(kWarn, "Then switch %s off on the main.", RName(r.satId).c_str());
    };
    if (Confirm("Return to normal##confirm", nullptr, "Return", body, rep != nullptr)) StartReturn(returnFor_);
  }
}

// Zone A: one card per dongle plugged in, like a character-select row. Each main comes first with its
// repeaters after it (an arrow under the cards: "repeater of"), then the rest. Click selects.
void Ui::DongleStrip(const DongleState& d) {
  const auto& all = manager_.Dongles();
  Section("DONGLES", infoOpen_, kInfoDongles, kInfoDonglesText);
  std::vector<DongleState> list;
  for (const auto& o : all) list.push_back(o->Snapshot());
  std::vector<size_t> mains, order;
  std::vector<bool> placed(list.size());
  for (size_t i = 0; i < list.size(); ++i)
    if (RoleOf(list[i]) == Role::Main) mains.push_back(i);
  std::stable_sort(mains.begin(), mains.end(), [&](size_t x, size_t y) { return list[x].paired > list[y].paired; });
  for (size_t m : mains) {
    order.push_back(m);
    placed[m] = true;
    for (size_t j = 0; j < list.size(); ++j)
      if (!placed[j] && RoleOf(list[j]) == Role::Repeater && list[m].netBase && list[j].mainBase == list[m].netBase) {
        order.push_back(j);
        placed[j] = true;
      }
  }
  for (int pass = 0; pass < 2; ++pass)   // repeaters without their main, then spares
    for (size_t j = 0; j < list.size(); ++j)
      if (!placed[j] && (pass == 1 || RoleOf(list[j]) == Role::Repeater)) { order.push_back(j); placed[j] = true; }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), fh = ImGui::GetFrameHeight(), px = Px();
  const float tlh = TitleHeight(), pad = fs * 0.6f, gap = fs * 1.1f, W = ImGui::GetContentRegionAvail().x;
  const float cw = fs * 14, ch = pad * 2 + tlh + lh + fh + fs * 0.4f, rowStep = ch + fs * 1.3f;
  const int perRow = std::max(1, int((W + gap) / (cw + gap)));
  const int rows = std::max(1, (int(order.size()) + perRow - 1) / perRow);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  std::map<std::string, std::pair<ImVec2, ImVec2>> rects;
  for (size_t n = 0; n < order.size(); ++n) {
    const DongleState& s = list[order[n]];
    const ImVec2 a(p.x + float(int(n) % perRow) * (cw + gap), p.y + float(int(n) / perRow) * rowStep), b(a.x + cw, a.y + ch);
    rects[s.serial] = {a, b};
    const bool me = s.serial == d.serial, old = s.open && !s.extras;
    const Role role = RoleOf(s);
    const ImVec4 rc = RoleColor(s);
    const float alpha = old ? 0.45f : 1;
    ImGui::PushID(s.serial.c_str());
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("card", ImVec2(cw, ch))) {
      dongle_ = s.serial;
      if (old) page_ = Page::Firmware;
    }
    const bool hover = ImGui::IsItemHovered();
    if (hover) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::SetTooltip("%s\nFirmware 0x%04X", s.serial.c_str(), s.firmware);
    }
    const float r = fs * 0.5f;
    dl->AddRectFilled(a, b, U32(lh::ui::CardColor()), r);
    if (me) dl->AddRectFilled(a, b, IM_COL32(110, 168, 255, 34), r);
    else if (hover) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 12), r);
    if (me) dl->AddRect(a, b, IM_COL32(235, 240, 248, 235), r, 0, 2 * px);
    else dl->AddRect(a, b, U32(rc, 0.45f), r, 0, px);
    // row 1: icon and role; row 2: serial; row 3: what the role shows
    float x = a.x + pad, y = a.y + pad;
    lh::ui::DrawIcon(dl, role == Role::Repeater ? Icon::Repeater : Icon::Dongle, ImVec2(x + tlh / 2, y + tlh / 2), tlh, U32(rc, alpha));
    const float tx = x + tlh + fs * 0.45f;
    std::string board = BoardWord(s.board);
    for (auto& ch : board) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    TextAt(dl, ImVec2(tx, y), rc, board.c_str(), g_title, alpha);   // (its role shows in the colour, the icon and row 3)
    y += tlh + fs * 0.2f;
    float sx = x;
    if (role == Role::Repeater && s.haveRole) {
      TextAt(dl, ImVec2(x, y), kAccent, RName(s.satId).c_str());
      sx += ImGui::CalcTextSize("R1").x + fs * 0.6f;
    }
    TextAt(dl, ImVec2(sx, y), kMuted, ShortSerial(s.serial).c_str(), nullptr, alpha);
    y += lh + fs * 0.2f;
    const float cy = y + fh / 2;
    if (!s.open) {
      TextAt(dl, ImVec2(x, cy - lh / 2), kBad, "Can't open");
    } else if (old) {   // the click goes to the Firmware page
      UpdateChip(dl, x, cy);
    } else if (!s.haveRole) {
      WaitDots(dl, ImVec2(x, cy), fs * 0.25f, kOff, now_);
    } else if (role == Role::Main || role == Role::Spare) {
      SlotPips(dl, ImVec2(x, cy), fs * 0.26f, s);
    } else if (role == Role::Repeater) {
      const auto main = FindByNetBase(all, s.mainBase, s.serial);
      if (!main) {
        TextAt(dl, ImVec2(x, cy - lh / 2), kOff, "Main unplugged");
      } else {
        const DongleState ms = main->Snapshot();
        const Sat st = SatState(s, &ms);
        const float w = LedGlyph(dl, ImVec2(x, cy), lh, st == Sat::Searching ? Led::Searching : Led::Following, now_, st == Sat::Starting ? 0.4f : 1);
        TextAt(dl, ImVec2(x + w + fs * 0.4f, cy - lh / 2), SatColor(st), SatWord(st));
      }
    }
    ImGui::PopID();
  }
  // "repeater of": an arrow under the cards, from each repeater to its main
  for (const auto& s : list) {
    if (RoleOf(s) != Role::Repeater) continue;
    const auto main = FindByNetBase(all, s.mainBase, s.serial);
    if (!main || !rects.count(main->Serial()) || !rects.count(s.serial)) continue;
    const auto [ra, rb] = rects[s.serial];
    const auto [ma, mb] = rects[main->Serial()];
    const ImVec2 from((ra.x + rb.x) / 2, rb.y + px * 2), to((ma.x + mb.x) / 2 + fs * 2, mb.y + px * 2);
    const float dip = fs * 0.9f;
    const ImU32 col = U32(kAccent, 0.75f);
    Curve(dl, from, ImVec2(from.x, from.y + dip), ImVec2(to.x, to.y + dip), ImVec2(to.x, to.y + fs * 0.5f), col, 2 * px, Stroke::Solid);
    Arrowhead(dl, to, 0, -1, fs * 0.55f, col);
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, float(rows) * rowStep - fs * 0.3f));
}

// The main's view: its repeater sockets, the devices between, and the before/after loss of each.
void Ui::RepeaterMain(const DongleState& d) {
  const auto dongle = Current();
  const auto& all = manager_.Dongles();
  const uint8_t mask = ShownMask(d);
  const uint8_t known = KnownIds(all, d) | mask;
  // repeaters of this main plugged in here, a spare that could become one
  std::array<std::string, kMaxRepeaters> who{};
  for (const auto& o : all) {
    const auto s = o->Snapshot();
    if (o->Serial() != d.serial && s.haveRole && s.role == 1 && s.mainBase == d.netBase && s.satId >= 0 && s.satId < kMaxRepeaters)
      who[s.satId] = s.serial;
  }
  std::array<Sock, kMaxRepeaters> sock{};
  bool anyOn = false, anyAnswer = false, anyLost = false;
  for (int k = 0; k < kMaxRepeaters; ++k) {
    sock[k] = SockFor(d, k, mask, known);
    anyOn |= (mask >> k) & 1;
    anyAnswer |= sock[k] == Sock::Live || sock[k] == Sock::Standby || sock[k] == Sock::Weak;
    anyLost |= sock[k] == Sock::Lost;
  }
  std::string made, madeMain;
  int madeId = 0;
  {
    std::lock_guard lock(resultsMutex_);
    made = repeaterMade_; madeMain = repeaterMadeMain_; madeId = repeaterMadeId_;
  }
  const bool setUp = !made.empty() && madeMain == d.serial;
  const bool madeOnUsb = setUp && FindBySerial(all, made);

  // the set-up steps: while this main has no repeater, or a set-up runs
  if (!known) {
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "No repeater. Any dongle with nothing connected nearby becomes one on its own.");
  }
  if (placementOpen_ && setUp) PlacementPanel(sock[madeId] == Sock::Lost && !madeOnUsb);

  Section("LINK MAP", infoOpen_, kInfoMapMain, kInfoMapMainText);
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), fh = ImGui::GetFrameHeight(), px = Px(), tlh = TitleHeight();
  const float W = ImGui::GetContentRegionAvail().x, pad = fs * 0.6f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float sockW = fs * 16, sockH = pad * 2 + tlh + fs * 0.3f + fh, sockGap = fs * 0.6f;
  const float bandH = sockH * kMaxRepeaters + sockGap * (kMaxRepeaters - 1), nodeW = fs * 9;
  const float sx = p.x + W - sockW;
  const ImVec2 na(p.x, p.y), nb(p.x + nodeW, p.y + bandH);
  const float ncy = (na.y + nb.y) / 2;

  // the main
  NodeCard(dl, na, nb, kInfo, "MAIN", ShortSerial(d.serial), &d);
  {
    char tip[96];
    if (d.haveTiming && d.cycleUs) std::snprintf(tip, sizeof tip, "Net 0x%08X\nCost %d us  %.0f %%", d.netBase, d.reserveUs, 100.0 * d.reserveUs / d.cycleUs);
    else std::snprintf(tip, sizeof tip, "Net 0x%08X\nCost %d us", d.netBase, d.reserveUs);
    AreaTip(na, nb, tip);
  }

  // the sockets and the backhaul to each
  for (int k = 0; k < kMaxRepeaters; ++k) {
    const Sock st = sock[k];
    const ImVec4 sc = SockColor(st);
    const auto& r = d.relaySats[k];
    const ImVec2 a(sx, p.y + float(k) * (sockH + sockGap)), b(sx + sockW, a.y + sockH);
    ImGui::PushID(k);
    if (st != Sock::Empty) {
      const ImVec2 b0(nb.x, ncy + (float(k) - 0.5f * (kMaxRepeaters - 1)) * fs * 1.2f), b3(a.x, (a.y + b.y) / 2);
      const float dx = (b3.x - b0.x) * 0.5f;
      const ImVec2 b1(b0.x + dx, b0.y), b2(b3.x - dx, b3.y);
      const Stroke style = st == Sock::Off ? Stroke::Dotted : st == Sock::Weak ? Stroke::Dashed : st == Sock::Lost ? Stroke::Broken : Stroke::Solid;
      Curve(dl, b0, b1, b2, b3, U32(sc, st == Sock::Off ? 0.6f : 0.85f), 2 * px, style);
      if (st == Sock::Live && r.acceptedPerSec > 0.5) Pulse(dl, b3, b2, b1, b0, kGood, now_, 0.6);
    }
    if (st == Sock::Empty) {
      DashedRect(dl, a, b, U32(kOff, 0.45f), px);
      const ImVec2 c(a.x + pad + tlh / 2, a.y + pad + tlh / 2);
      dl->AddCircle(c, tlh * 0.42f, U32(kOff, 0.8f), 16, 1.5f * px);
      dl->AddLine(ImVec2(c.x - tlh * 0.2f, c.y), ImVec2(c.x + tlh * 0.2f, c.y), U32(kOff), 1.5f * px);
      dl->AddLine(ImVec2(c.x, c.y - tlh * 0.2f), ImVec2(c.x, c.y + tlh * 0.2f), U32(kOff), 1.5f * px);
      TextAt(dl, ImVec2(c.x + tlh / 2 + fs * 0.45f, a.y + pad), kOff, RName(k).c_str(), g_title);
      const float y = a.y + pad + tlh + fs * 0.3f;
      TextAt(dl, ImVec2(a.x + pad, y + (fh - lh) / 2), kOff, "Empty");
      ImGui::PopID();
      continue;
    }
    Card(dl, a, b, sc, st == Sock::Off ? 0.4f : 0.75f, st == Sock::Off ? 1.f : 1.5f);
    // row 1: icon, R1, (?) and the NO ANSWER mark
    const float iy = a.y + pad;
    const bool wireless = d.autoMask >> k & 1;
    const auto& ar = d.autoReps[k];
    const std::string title = wireless && !ar.serial.empty() ? std::string(BoardWord(ar.board)) + " " + ar.serial : RName(k);
    lh::ui::DrawIcon(dl, wireless && who[k].empty() ? Icon::Wireless : who[k].empty() ? Icon::Repeater : Icon::Wire,
                     ImVec2(a.x + pad + tlh / 2, iy + tlh / 2), tlh, U32(kAccent, st == Sock::Off ? 0.6f : 1));
    TextAt(dl, ImVec2(a.x + pad + tlh + fs * 0.45f, iy), kAccent, title.c_str(), wireless ? nullptr : g_title);
    ImGui::SetCursorScreenPos(ImVec2(b.x - pad - lh, iy + (tlh - lh) / 2));
    InfoHover("switch", kInfoSwitchText);
    if (st == Sock::Lost && std::fmod(now_, 1.0) < 0.6) {
      const ImVec2 c(b.x - pad - lh - fs * 1.1f, iy + tlh / 2);
      dl->AddCircleFilled(c, lh * 0.5f, U32(kBad), 16);
      TextCentered(dl, c, ImVec4(0.08f, 0.09f, 0.12f, 1), "!");
    }
    // row 2: the switch and ON / OFF; the state and its pips at the right
    const float ry = iy + tlh + fs * 0.3f;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ry));
    const bool on = mask & (1u << k);
    if (wireless) {   // joined on its own: the main keeps it while it answers, no switch
      TextAt(dl, ImVec2(a.x + pad, ry + (fh - lh) / 2), kMuted, who[k].empty() ? "WIRELESS" : "USB");
    } else if (PowerSwitch("power", on, on ? (st == Sock::Lost ? kBad : kGood) : kOff)) {
      const uint8_t next = uint8_t(on ? mask & ~(1u << k) : mask | (1u << k));
      maskPending_[d.serial] = {next, now_ + 3};
      const std::string word = RName(k) + (on ? " off" : " on");
      dongle->SetRepeaters(next, false, [this, word](bool ok, const std::string& msg) { Post(ok ? word : "Switch failed: " + msg, ok); });
    }
    if (!wireless) {
      const float swEnd = ImGui::GetItemRectMax().x;
      TextAt(dl, ImVec2(swEnd + fs * 0.4f, ry + (fh - lh) / 2), on ? kText : kOff, on ? "ON" : "OFF");
    }
    if (st != Sock::Off) {
      const bool pips = st != Sock::Lost;
      const float r2 = fs * 0.22f, pipsW = pips ? r2 * 2.7f * 3 : 0;
      const float wx = b.x - pad - pipsW;
      const float ww = ImGui::CalcTextSize(SockWord(st)).x;
      TextAt(dl, ImVec2(wx - ww - (pips ? fs * 0.4f : 0), ry + (fh - lh) / 2), sc, SockWord(st));
      if (st == Sock::Live || st == Sock::Weak) HealthPips(dl, ImVec2(wx, ry + fh / 2), r2, st == Sock::Weak ? 1 : AnswerPips(r), sc, false);
      else if (st == Sock::Standby) HealthPips(dl, ImVec2(wx, ry + fh / 2), r2, 0, sc, true);
    }
    char tip[320];
    const std::string last = r.msSinceSrep == 0xFFFFFFFFu ? "-" : [&] { char t[24]; std::snprintf(t, sizeof t, "%.1f s", r.msSinceSrep / 1000.0); return std::string(t); }();
    std::snprintf(tip, sizeof tip, "Last answer %s\nAsked %.0f/s\nAnswered %.0f/s\nTaken %.0f/s%s%s", last.c_str(), r.asksPerSec, r.srepsPerSec,
                  r.acceptedPerSec, who[k].empty() ? "" : "\n", who[k].c_str());
    if (wireless) {
      const size_t n = std::strlen(tip);
      std::snprintf(tip + n, sizeof tip - n, "\nJoined on its own\nHears the main at %d dBm\nHeard at %d dBm%s", ar.hearsMainRssi, ar.heardRssi,
                    d.uselessFor ? "\nNot helping: it hears too little of the devices. Move it" : d.relaying ? "" : "\nStandby: nothing is lost, so it isn't asked");
    }
    AreaTip(a, b, tip);
    ImGui::PopID();
  }

  // the device rows: left link to the main, the device, the loss bar, right link to the repeaters
  std::vector<int> rows;
  for (int i = 0; i < kSlots; ++i)
    if (d.slots[i].used) rows.push_back(i);
  enum class Right { None, Off, Grey, Live } right = anyAnswer ? Right::Live : anyLost ? Right::Grey : known ? Right::Off : Right::None;
  if (!anyOn && right != Right::None) right = Right::Off;
  const float headY = p.y + bandH + fs * 0.7f, rowY0 = headY + lh + fs * 0.2f, rowH = fh;
  const float xBusL = (na.x + nb.x) / 2, xIcon = nb.x + fs * 0.7f, xBusR = sx + sockW / 2;
  const float lossTextW = fs * 5.5f;
  const float xName = xIcon + lh + fs * 2.0f;
  const float nameW = std::clamp(xBusR - xName - fs * 11 - lossTextW - fs * 9, fs * 7, fs * 15);
  const float lossW = std::clamp(xBusR - xName - nameW - lossTextW - fs * 11, fs * 11, fs * 17);
  const float xBar = xName + nameW + fs * 0.5f, xBarEnd = xBar + lossW, xLossText = xBarEnd + fs * 0.6f, xRight = xLossText + lossTextW;
  const float lastY = rows.empty() ? rowY0 : rowY0 + (float(rows.size()) - 0.5f) * rowH;
  if (!rows.empty()) {
    TextCentered(dl, ImVec2((xBusL + xIcon) / 2, headY + lh / 2), kMuted, "at main");
    TextAt(dl, ImVec2(xBar, headY), kMuted, "loss");
    if (right != Right::None) TextCentered(dl, ImVec2((xRight + xBusR) / 2, headY + lh / 2), kMuted, "at repeater");
    dl->AddLine(ImVec2(xBusL, nb.y), ImVec2(xBusL, lastY), U32(kInfo, 0.55f), 2 * px);
    if (right != Right::None)
      Line(dl, ImVec2(xBusR, p.y + bandH), ImVec2(xBusR, lastY), U32(right == Right::Live ? kAccent : kOff, right == Right::Live ? 0.6f : 0.45f), 2 * px,
           right == Right::Off ? Stroke::Dotted : Stroke::Solid);
  } else {
    TextAt(dl, ImVec2(xIcon, headY), kOff, "No devices paired");
  }
  const auto& ab = abMeans_[d.serial];
  for (size_t n = 0; n < rows.size(); ++n) {
    const int i = rows[n];
    const auto& s = d.slots[i];
    const float y0 = rowY0 + float(n) * rowH, cy = y0 + rowH / 2;
    const float dim = s.active ? 1.f : 0.45f;
    const ImVec2 la(xBusL, cy), lb(xIcon - fs * 0.35f, cy);
    if (s.active) {
      SignalLink(dl, la, lb, s.rssiDongle, (la.x + lb.x) / 2, lh * 0.75f, rowH / 2);
    } else {
      Line(dl, la, lb, U32(kOff, 0.5f), px, Stroke::Dotted);
    }
    // the device
    lh::ui::DrawIcon(dl, Icon::Tracker, ImVec2(xIcon + lh / 2, cy), lh, U32(s.active ? kText : kOff, s.active ? 0.85f : 0.6f));
    char num[8];
    std::snprintf(num, sizeof num, "%d", i);
    TextAt(dl, ImVec2(xIcon + lh + fs * 0.5f, cy - lh / 2), kMuted, num, nullptr, dim);
    const std::string name = DeviceLabel(s);
    dl->PushClipRect(ImVec2(xName, y0), ImVec2(xName + nameW, y0 + rowH), true);
    TextAt(dl, ImVec2(xName, cy - lh / 2), s.active ? kText : kOff, name.c_str());
    dl->PopClipRect();
    if (ImGui::CalcTextSize(name.c_str()).x > nameW) AreaTip(ImVec2(xName, y0), ImVec2(xName + nameW, y0 + rowH), name);
    // loss: before / after
    const ImVec2 ba(xBar, cy - lh * 0.38f), bb(xBarEnd, cy + lh * 0.38f);
    const double polls = s.pollsPerSec;
    // 0x23's answers include every answer a repeater relayed: that's the loss the driver sees. The main's
    // own loss adds them back.
    const double own = s.lossPercent < 0 || polls <= 0 ? -1 : std::min(100.0, s.lossPercent + 100.0 * d.relayRelayedPerSec[i] / polls);
    if (!s.active) {
      dl->AddRectFilled(ba, bb, U32(kTrack, 0.6f), (bb.y - ba.y) * 0.3f);
      TextAt(dl, ImVec2(ba.x + fs * 0.5f, cy - lh / 2), kOff, "off");
    } else {
      LossBar(dl, ba, bb, own, s.lossPercent);
      char t[32];
      if (own < 0) std::snprintf(t, sizeof t, "-");
      else if (anyOn) std::snprintf(t, sizeof t, "%.0f", own);
      else std::snprintf(t, sizeof t, "%.0f %%", own);
      TextAt(dl, ImVec2(xLossText, cy - lh / 2), own < 0 ? kOff : LossColor(own), t);
      if (anyOn && own >= 0) {   // "9 > 1 %": an arrow glyph between
        const float tx = xLossText + ImGui::CalcTextSize(t).x + fs * 0.35f, ar = lh * 0.22f;
        dl->AddTriangleFilled(ImVec2(tx, cy - ar), ImVec2(tx + ar * 1.4f, cy), ImVec2(tx, cy + ar), U32(kMuted));
        char m[16];
        std::snprintf(m, sizeof m, "%.0f %%", std::max(0.0, s.lossPercent));
        TextAt(dl, ImVec2(tx + ar * 1.4f + fs * 0.35f, cy - lh / 2), LossColor(s.lossPercent), m);
      }
      std::string tip;
      char t2[96];
      std::snprintf(t2, sizeof t2, "Main alone %.1f %%", std::max(0.0, own));
      tip = t2;
      if (anyOn) {
        std::snprintf(t2, sizeof t2, "\nWith repeater %.1f %%\nSaved %.0f of %.0f /s", std::max(0.0, s.lossPercent), d.relayAcceptedPerSec[i],
                      polls * std::max(0.0, own) / 100);   // of what the main missed
        tip += t2;
      }
      for (int bkt = 0; bkt < 2; ++bkt)
        if (ab.n[bkt][i] >= 5) {
          std::snprintf(t2, sizeof t2, "\nMean %s %.1f %%", bkt == 0 ? "ON " : "OFF", ab.sum[bkt][i] / ab.n[bkt][i]);
          tip += t2;
        }
      AreaTip(ImVec2(xBar, y0), ImVec2(xRight, y0 + rowH), tip);
    }
    // right link to the repeaters
    if (right != Right::None) {
      const ImVec2 ra(xRight, cy), rb(xBusR, cy);
      const int rr = d.relaySatRssi[i];
      if (right == Right::Live && s.active && rr) {
        SignalLink(dl, ra, rb, rr, rb.x - fs * 3, lh * 0.75f, rowH / 2);
        if (d.relayAcceptedPerSec[i] > 0.5) PulseLine(dl, ra, rb, kGood, now_ + i * 0.37, 0.7);
      } else {
        Line(dl, ra, rb, U32(kOff, right == Right::Grey ? 0.55f : 0.4f), px, right == Right::Grey && s.active ? Stroke::Solid : Stroke::Dotted);
      }
    }
  }
  const float mapH = (rows.empty() ? headY + lh : rowY0 + float(rows.size()) * rowH) - p.y;
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, mapH));

  // legend and the action row
  ImGui::Spacing();
  {
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float sw = fs * 1.6f, sh = lh * 0.6f, y = q.y + (fh - sh) / 2;
    float x = q.x;
    const auto item = [&](int kind, const char* text) {
      const ImVec2 a(x, y), b(x + sw, y + sh);
      if (kind == 0) dl->AddRect(a, b, U32(kMuted, 0.8f), sh * 0.3f, 0, px);
      else if (kind == 1) SavedFill(dl, a, b, sh * 0.3f, ImDrawFlags_RoundCornersAll);
      else dl->AddRectFilled(a, b, U32(kWarn, 0.95f), sh * 0.3f);
      TextAt(dl, ImVec2(b.x + fs * 0.4f, q.y + (fh - lh) / 2), kMuted, text);
      x = b.x + fs * 0.4f + ImGui::CalcTextSize(text).x + fs * 1.4f;
    };
    if (!rows.empty()) {
      dl->PushClipRect(ImVec2(q.x, q.y), ImVec2(q.x + W, q.y + fh), true);
      item(0, "main alone");
      if (anyOn) item(1, "saved");
      item(2, "still lost");
      dl->PopClipRect();
      ImGui::SetCursorScreenPos(ImVec2(x, q.y));
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("0 to 30 %%");
      ImGui::SetCursorScreenPos(q);
    }
    const float bw = fs * 9;
    ImGui::SetCursorScreenPos(ImVec2(q.x + W - bw, q.y));
    if (ImGui::Button("Placement", ImVec2(bw, 0))) placementOpen_ = !placementOpen_;
    ImGui::SetCursorScreenPos(q);
    ImGui::Dummy(ImVec2(W - bw - fs, fh));
  }
  if (placementOpen_ && !setUp) PlacementPanel(false);
}

// A dongle with nothing connected: it becomes a repeater by itself near a dongle that has devices.
void Ui::RepeaterSpare(const DongleState& d) {
  (void)d;
  ImGui::Spacing();
  ImGui::TextColored(kMuted, "Nothing connected. Becomes a repeater on its own when it hears a dongle with devices.");
}

// The repeater's own view, on USB: what it hears, the same as on its charger.
void Ui::RepeaterSelf(const DongleState& d) {
  const auto& all = manager_.Dongles();
  const auto main = FindByNetBase(all, d.mainBase, d.serial);
  const DongleState ms = main ? main->Snapshot() : DongleState{};
  const Sat sat = SatState(d, main ? &ms : nullptr);
  const ImVec4 sc = SatColor(sat);
  if (placementOpen_) PlacementPanel(false);

  Section("LINK MAP", infoOpen_, kInfoMapRep, kInfoMapRepText);
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), fh = ImGui::GetFrameHeight(), px = Px(), tlh = TitleHeight();
  const float W = ImGui::GetContentRegionAvail().x, pad = fs * 0.6f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float nodeW = fs * 9, repW = fs * 15, bandH = fs * 7.2f;
  const ImVec2 na(p.x, p.y), nb(p.x + nodeW, p.y + bandH), ra(p.x + W - repW, p.y), rb(p.x + W, p.y + bandH);
  const float cy = p.y + bandH / 2;
  // the main
  {
    NodeCard(dl, na, nb, kInfo, "MAIN", ShortSerial(ms.serial), nullptr, nullptr, main ? nullptr : "Unplugged");
    char tip[48];
    std::snprintf(tip, sizeof tip, "Net 0x%08X", d.mainBase);
    AreaTip(na, nb, tip);
  }
  // the backhaul
  {
    const ImVec2 b0(nb.x, cy), b3(ra.x, cy);
    const float mx = (b0.x + b3.x) / 2;
    if (sat == Sat::Searching) {
      Line(dl, b0, b3, U32(kWarn, 0.55f), 1.5f * px, Stroke::Dotted);
      dl->AddCircleFilled(ImVec2(mx, cy), lh * 0.75f, U32(ImVec4(0.067f, 0.075f, 0.098f, 1)), 16);
      TextCentered(dl, ImVec2(mx, cy), kWarn, "?", g_title);
      Radar(dl, ImVec2(ra.x, cy), fs * 6, kWarn, now_);
    } else if (sat == Sat::Following) {
      SignalLink(dl, b0, b3, d.satMainRssi, mx, lh * 0.9f, lh);
      PulseLine(dl, b0, b3, kGood, now_, 0.7);
    } else if (sat == Sat::OffAtMain) {
      dl->AddLine(b0, b3, U32(kOff, 0.7f), 2 * px);
    }
  }
  // the repeater
  {
    Card(dl, ra, rb, sc, 0.8f, 1.5f);
    float y = ra.y + pad;
    const float x = ra.x + pad;
    lh::ui::DrawIcon(dl, Icon::Repeater, ImVec2(x + tlh / 2, y + tlh / 2), tlh, U32(kAccent));
    TextAt(dl, ImVec2(x + tlh + fs * 0.45f, y), kAccent, RName(d.satId).c_str(), g_title);
    ImGui::SetCursorScreenPos(ImVec2(rb.x - pad - lh, y + (tlh - lh) / 2));
    InfoHover("satinfo", kInfoMapRepText);
    y += tlh + fs * 0.5f;
    if (sat == Sat::Starting) WaitDots(dl, ImVec2(x, y + lh / 2), fs * 0.25f, kOff, now_);
    else dl->AddCircleFilled(ImVec2(x + fs * 0.3f, y + lh / 2), fs * 0.3f, U32(sc), 12);
    TextAt(dl, ImVec2(x + fs * (sat == Sat::Starting ? 1.6f : 1.0f), y), sc, SatWord(sat), g_title);
    y += tlh + fs * 0.5f;
    TextAt(dl, ImVec2(x, y), kMuted, "LED");
    LedGlyph(dl, ImVec2(x + fs * 2.4f, y + lh / 2), lh, sat == Sat::Searching ? Led::Searching : Led::Following, now_, sat == Sat::Starting ? 0.35f : 1);
    char tip[200];
    std::snprintf(tip, sizeof tip, "Channel %d\n%.0f cycles/s\nAsked %.0f/s\nAnswered %.0f/s\nNet 0x%08X", d.satChannel,
                  d.satCyclesPerSec, d.satNeedsPerSec, d.satSrepsPerSec, d.netBase);
    AreaTip(ra, rb, tip);
  }
  // the devices: what the repeater hears of each
  std::vector<int> rows;
  for (int i = 0; i < kSlots; ++i)
    if ((main && ms.slots[i].used) || d.satHeardPerSec[i] >= 0.5) rows.push_back(i);
  const float headY = p.y + bandH + fs * 0.7f, rowY0 = headY + lh + fs * 0.2f, rowH = fh;
  const float xIcon = nb.x + fs * 0.7f, xName = xIcon + lh + fs * 2.0f, xBusR = ra.x + repW / 2;
  const float textW = fs * 4;
  const float nameW = std::clamp(xBusR - xName - fs * 11 - textW - fs * 9, fs * 7, fs * 15);
  const float barW = std::clamp(xBusR - xName - nameW - textW - fs * 11, fs * 11, fs * 17);
  const float xBar = xName + nameW + fs * 0.5f, xBarEnd = xBar + barW, xText = xBarEnd + fs * 0.6f, xRight = xText + textW;
  const bool hearing = sat == Sat::Following || sat == Sat::OffAtMain;
  if (!rows.empty()) {
    TextAt(dl, ImVec2(xBar, headY), kMuted, "heard");
    const float lastY = rowY0 + (float(rows.size()) - 0.5f) * rowH;
    Line(dl, ImVec2(xBusR, rb.y), ImVec2(xBusR, lastY), U32(hearing ? kAccent : kOff, 0.5f), 2 * px, hearing ? Stroke::Solid : Stroke::Dotted);
  }
  for (size_t n = 0; n < rows.size(); ++n) {
    const int i = rows[n];
    const float y0 = rowY0 + float(n) * rowH, rcy = y0 + rowH / 2;
    const double frac = d.satCyclesPerSec > 0 ? std::min(1.0, d.satHeardPerSec[i] / d.satCyclesPerSec) : 0;
    const bool off = main && ms.slots[i].used && !ms.slots[i].active && frac <= 0;   // the device is off
    const bool heard = hearing && frac > 0;
    lh::ui::DrawIcon(dl, Icon::Tracker, ImVec2(xIcon + lh / 2, rcy), lh, U32(heard ? kText : kOff, heard ? 0.85f : 0.6f));
    char num[8];
    std::snprintf(num, sizeof num, "%d", i);
    TextAt(dl, ImVec2(xIcon + lh + fs * 0.5f, rcy - lh / 2), kMuted, num);
    std::string name;
    if (main && ms.slots[i].used) name = DeviceLabel(ms.slots[i]);
    else { char t[16]; std::snprintf(t, sizeof t, "Slot %d", i); name = t; }
    dl->PushClipRect(ImVec2(xName, y0), ImVec2(xName + nameW, y0 + rowH), true);
    TextAt(dl, ImVec2(xName, rcy - lh / 2), heard ? kText : kOff, name.c_str());
    dl->PopClipRect();
    const ImVec2 ba(xBar, rcy - lh * 0.38f), bb(xBarEnd, rcy + lh * 0.38f);
    HeardBar(dl, ba, bb, hearing ? frac : 0);
    if (off) {
      TextAt(dl, ImVec2(ba.x + fs * 0.5f, rcy - lh / 2), kOff, "off");
    } else if (hearing && d.satCyclesPerSec > 0) {
      char t[16];
      std::snprintf(t, sizeof t, "%.0f %%", 100 * frac);
      TextAt(dl, ImVec2(xText, rcy - lh / 2), frac > 0.9 ? kGood : frac > 0.6 ? kWarn : kBad, t);
      char tip[48];
      std::snprintf(tip, sizeof tip, "%.0f of %.0f /s", d.satHeardPerSec[i], d.satCyclesPerSec);
      AreaTip(ImVec2(xBar, y0), ImVec2(xRight, y0 + rowH), tip);
    }
    const ImVec2 la(xRight, rcy), lb(xBusR, rcy);
    const int rr = d.satHeardRssi[i];
    if (heard && rr) {
      SignalLink(dl, la, lb, rr, lb.x - fs * 3, lh * 0.75f, rowH / 2);
    } else if (hearing && !off) {
      Line(dl, la, lb, U32(kOff, 0.4f), px, Stroke::Dotted);
    }
  }
  const float mapH = (rows.empty() ? headY : rowY0 + float(rows.size()) * rowH) - p.y;
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, mapH));

  // the action row
  ImGui::Spacing();
  {
    const float bw = fs * 9, rw = fs * 12, gapX = ImGui::GetStyle().ItemSpacing.x, ls = ImGui::GetTextLineHeight();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + W - bw - rw - gapX * 2 - ls);
    if (BigButton("Placement", bw)) placementOpen_ = !placementOpen_;
    ImGui::SameLine();
    if (TintButton("Return to normal", kBad, rw)) { returnFor_ = d.serial; openReturn_ = true; }
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() * 1.25f - ls) / 2);
    InfoHover("returninfo", kInfoReturnText);
  }
}

// Where to put the repeater: four tiles. Opens by itself after a link; the Placement button reopens it.
void Ui::PlacementPanel(bool tooFar) {
  bool close = false;
  Section("PLACE IT", infoOpen_, kInfoPlace, kInfoPlaceText, &close);
  if (close) {
    placementOpen_ = false;
    // the repeater already left USB: the set-up is over from here on
    std::lock_guard lock(resultsMutex_);
    if (!repeaterMade_.empty() && !FindBySerial(manager_.Dongles(), repeaterMade_)) repeaterMade_.clear();
    return;
  }
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), px = Px(), tlh = TitleHeight();
  const float W = ImGui::GetContentRegionAvail().x, gap = fs * 0.8f, th = fs * 7.3f;
  const float tw = std::min(fs * 10, (W - gap * 3) / 4.6f), lw = W - (tw + gap) * 3;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const auto tile = [&](int i, float w, const ImVec4& edge) {
    const ImVec2 a(p.x + float(i) * (tw + gap), p.y), b(a.x + w, a.y + th);
    Card(dl, a, b, edge, 0.5f, edge.x == kBad.x ? 2.f : 1.f);
    if (i < 3) {
      char num[4];
      std::snprintf(num, sizeof num, "%d", i + 1);
      TextAt(dl, ImVec2(a.x + fs * 0.6f, a.y + fs * 0.4f), kMuted, num, g_title);
    }
    return std::make_pair(a, b);
  };
  const ImU32 ink = U32(kAccent);
  {   // 1: unplug
    const auto [a, b] = tile(0, tw, kOff);
    const float cx = (a.x + b.x) / 2, cy = a.y + th * 0.47f, s = fs * 2;
    lh::ui::DrawIcon(dl, Icon::Dongle, ImVec2(cx - fs * 1.9f, cy), s, ink);
    dl->AddLine(ImVec2(cx - fs * 1.9f, cy - s * 0.45f), ImVec2(cx - fs * 1.9f, cy - s * 0.62f), ink, 2 * px);
    const float y = cy - s * 0.62f;
    dl->AddLine(ImVec2(cx - fs * 1.9f, y), ImVec2(cx - fs * 0.5f, y), ink, 2 * px);
    for (float k : {-0.25f, 0.35f}) dl->AddLine(ImVec2(cx + fs * k - fs * 0.15f, y + fs * 0.35f), ImVec2(cx + fs * k + fs * 0.15f, y - fs * 0.35f), U32(kText), 2 * px);
    dl->AddLine(ImVec2(cx + fs * 0.65f, y), ImVec2(cx + fs * 1.6f, y), U32(kOff), 2 * px);
    lh::ui::DrawIcon(dl, Icon::Plug, ImVec2(cx + fs * 2.1f, y + fs * 0.2f), fs * 1.2f, U32(kOff));
    TextCentered(dl, ImVec2(cx, b.y - lh), kText, "Unplug");
  }
  {   // 2: charger
    const auto [a, b] = tile(1, tw, kOff);
    const float cx = (a.x + b.x) / 2, cy = a.y + th * 0.47f, s = fs * 2;
    lh::ui::DrawIcon(dl, Icon::Dongle, ImVec2(cx - fs * 1.4f, cy), s, ink);
    lh::ui::DrawIcon(dl, Icon::Charger, ImVec2(cx + fs * 1.4f, cy), s, U32(kText, 0.85f));
    dl->AddLine(ImVec2(cx - fs * 0.85f, cy), ImVec2(cx + fs * 0.75f, cy), U32(kGood), 3 * px);
    TextCentered(dl, ImVec2(cx, b.y - lh), kText, "Charger");
  }
  {   // 3: across the room, top-down
    const ImVec4 edge = tooFar ? kBad : kOff;
    const auto [a, b] = tile(2, tw, edge);
    const ImVec2 ra(a.x + fs * 2.2f, a.y + fs * 0.7f), rb(b.x - fs * 0.8f, b.y - lh * 1.9f);
    dl->AddRect(ra, rb, U32(kMuted, 0.5f), 2 * px, 0, px);
    const ImVec2 mc(ra.x + fs * 0.75f, rb.y - fs * 0.75f), rc(rb.x - fs * 0.75f, ra.y + fs * 0.75f);
    Line(dl, mc, rc, U32(tooFar ? kBad : kOff, 0.8f), 1.5f * px, Stroke::Dotted);
    dl->AddCircleFilled(mc, fs * 0.55f, U32(kInfo), 16);
    TextCentered(dl, mc, ImVec4(0.08f, 0.09f, 0.12f, 1), "M");
    dl->AddCircleFilled(rc, fs * 0.55f, U32(kAccent), 16);
    TextCentered(dl, rc, ImVec4(0.08f, 0.09f, 0.12f, 1), "R");
    TextAt(dl, ImVec2(ra.x + fs * 0.3f, ra.y + fs * 0.2f), kMuted, "head high");
    TextCentered(dl, ImVec2((a.x + b.x) / 2, b.y - lh), tooFar ? kBad : kText, tooFar ? "Too far" : "Across room");
  }
  {   // the LED, live
    const auto [a, b] = tile(3, lw, kOff);
    TextAt(dl, ImVec2(a.x + fs * 0.6f, a.y + fs * 0.4f), kMuted, "LED", g_title);
    const float y1 = a.y + fs * 0.4f + tlh + fs * 0.7f, step = lh * 1.45f, x = a.x + fs * 0.8f;
    const float w = LedGlyph(dl, ImVec2(x, y1), lh * 1.2f, Led::Following, now_);
    TextAt(dl, ImVec2(x + w + fs * 0.8f, y1 - lh / 2), kGood, "Following");
    LedGlyph(dl, ImVec2(x, y1 + step), lh * 1.2f, Led::Searching, now_);
    TextAt(dl, ImVec2(x + w + fs * 0.8f, y1 + step - lh / 2), kWarn, "Searching");
    LedGlyph(dl, ImVec2(x, y1 + step * 2), lh * 1.2f, Led::Normal, now_);
    TextAt(dl, ImVec2(x + w + fs * 0.8f, y1 + step * 2 - lh / 2), kOff, "Not a repeater");
  }
  ImGui::Dummy(ImVec2(W, th));
}

// ---- Timing (0x2D, firmware that answers 0x3F) ----
// One scene, three zones: the loadout (a card per mode, with a picture of the mode's cycle), the live cycle
// (the running cycle to scale) and the devices. Same components and colours as the Repeater page: the
// dongle's POLL is blue, a device's answer warm.

namespace {
constexpr uint32_t kInfoLoadout = 32, kInfoCycle = 64, kInfoTimingDevices = 128;

// "5 592 us"
std::string Us(uint32_t us) {
  char t[32];
  if (us >= 1000) std::snprintf(t, sizeof t, "%u %03u us", us / 1000, us % 1000);
  else std::snprintf(t, sizeof t, "%u us", us);
  return t;
}

// The picture of an offered mode's cycle (1, 3): three devices, adaptive slots, one time scale for both (a
// shorter picture is a shorter cycle). Illustration constants, not live data.

// A start mark over a POLL: filled = started by the hardware timer, hollow = started by software.
void StartMark(ImDrawList* dl, ImVec2 top, float s, bool timer, ImU32 col) {
  const ImVec2 t0(top.x - s * 0.55f, top.y), t1(top.x + s * 0.55f, top.y), t2(top.x, top.y + s);
  if (timer) dl->AddTriangleFilled(t0, t1, t2, col);
  else dl->AddTriangle(t0, t1, t2, col, Px());
}
// playhead 0..1: a line sweeping the picture (< 0: none).
void CycleGlyph(ImDrawList* dl, ImVec2 a, ImVec2 b, int mode, float playhead, float alpha = 1) {
  if (!TimingModeOffered(mode)) return;
  const bool timer = mode == 3;
  const float px = Px(), full = b.x - a.x, H = b.y - a.y;
  const float W = full * (timer ? 0.68f : 0.70f), markH = H * 0.32f, y0 = a.y + markH, y1 = b.y;
  const float ry0 = y0 + (y1 - y0) * 0.18f, ry1 = y1 - (y1 - y0) * 0.18f;
  const ImU32 blue = U32(kInfo, 0.9f * alpha), warm = U32(kAccent, 0.85f * alpha), edge = U32(kMuted, 0.75f * alpha), ink = U32(kText, 0.85f * alpha);
  dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(a.x + W, y1), U32(kTrack, alpha), 2 * px);
  const auto slotEdge = [&](float x1) { Line(dl, ImVec2(x1, y0), ImVec2(x1, y1), edge, px, Stroke::Dotted); };
  const float gap = full * 0.01f, sw = W / 3, pollW = full * 0.035f, replyW = full * 0.14f;
  for (int i = 0; i < 3; ++i) {
    const float x = a.x + sw * float(i);
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + pollW, y1), blue);
    dl->AddRectFilled(ImVec2(x + pollW + gap, ry0), ImVec2(x + pollW + gap + replyW, ry1), warm);
    StartMark(dl, ImVec2(x + pollW / 2, a.y), markH * 0.85f, timer, ink);
    slotEdge(x + sw);
  }
  if (playhead >= 0) {
    const float x = a.x + W * playhead;
    dl->AddLine(ImVec2(x, a.y), ImVec2(x, y1), U32(kText, 0.9f * alpha), 1.5f * px);
  }
}

// The glyph legend, inside the LOADOUT info block: a swatch and a word each, wrapped to the width.
void GlyphLegend() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), px = Px();
  ImGui::Dummy(ImVec2(0, lh * 0.35f));
  const float W = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float sw = fs * 1.4f, sh = lh * 0.75f, rowH = lh * 1.5f;
  float x = p.x, y = p.y;
  const char* words[] = {"dongle asks", "device answers", "started by timer", "started by software", "width adapts"};
  for (int k = 0; k < 5; ++k) {
    const float w = sw + fs * 0.4f + ImGui::CalcTextSize(words[k]).x + fs * 1.4f;
    if (x > p.x && x + w > p.x + W) { x = p.x; y += rowH; }
    const ImVec2 a(x, y + (lh - sh) / 2), b(x + sw, a.y + sh);
    switch (k) {
      case 0: dl->AddRectFilled(ImVec2(a.x + sw * 0.35f, a.y), ImVec2(a.x + sw * 0.6f, b.y), U32(kInfo, 0.9f)); break;
      case 1: dl->AddRectFilled(a, b, U32(kAccent, 0.85f)); break;
      case 2: StartMark(dl, ImVec2((a.x + b.x) / 2, a.y), sh, true, U32(kText, 0.85f)); break;
      case 3: StartMark(dl, ImVec2((a.x + b.x) / 2, a.y), sh, false, U32(kText, 0.85f)); break;
      default:
        dl->AddRectFilled(ImVec2(a.x, a.y + sh * 0.18f), ImVec2(a.x + sw * 0.55f, b.y - sh * 0.18f), U32(kAccent, 0.85f));
        Line(dl, ImVec2(b.x - 2 * px, a.y - px), ImVec2(b.x - 2 * px, b.y + px), U32(kText), 1.5f * px, Stroke::Dotted);
        break;
    }
    TextAt(dl, ImVec2(b.x + fs * 0.4f, y), kText, words[k]);
    x += w;
  }
  ImGui::Dummy(ImVec2(W, y + lh - p.y));
}

// A mode card's state.
enum class CardState { Reading, Running, Switching, Leaving, Idle, Locked };
CardState ModeCardState(const DongleState& d, int m) {
  if (!d.extras) return CardState::Locked;
  if (!d.haveTiming) return CardState::Reading;
  if (d.modeNow == m && d.modeRequested == m) return CardState::Running;
  if (d.modeRequested == m) return CardState::Switching;
  if (d.modeNow == m) return CardState::Leaving;
  return CardState::Idle;
}

// The pin ("keep this mode on this dongle"): a round head on a needle, filled when pinned.
bool PinGlyph(const char* id, ImVec2 at, float s, bool pinned, const ImVec4& c) {
  ImGui::SetCursorScreenPos(at);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
  const bool hover = ImGui::IsItemHovered();
  if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float px = Px(), r = s * 0.24f;
  const ImVec2 head(at.x + s * 0.5f, at.y + s * 0.32f);
  const ImU32 col = U32(c, hover ? 1.f : pinned ? 0.95f : 0.6f);
  dl->AddLine(ImVec2(head.x, head.y + r), ImVec2(head.x, at.y + s * 0.95f), col, 1.5f * px);
  if (pinned) dl->AddCircleFilled(head, r, col, 16);
  else dl->AddCircle(head, r, col, 16, 1.5f * px);
  return clicked;
}

// The running cycle as a list of parts. The reads it comes from (0x2D every 2 s, the slots every 1 s) can
// disagree around a mode change, so nothing asserts: a sum above cycleUs scales the bar to the sum.
enum class Seg { Device, Join, Reserve, Rest };
struct CycleSeg { Seg kind; int slot; uint32_t us; std::string label; };
struct Cycle { std::vector<CycleSeg> segs; uint32_t total = 0; };
Cycle BuildCycle(const DongleState& d) {
  Cycle c;
  for (int i = 0; i < kSlots; ++i)
    if (d.slots[i].active) c.segs.push_back({Seg::Device, i, d.slotUs, std::to_string(i)});
  const bool reserve = d.haveRole && d.role == 0 && d.satMask && d.reserveUs > 0;
  c.segs.push_back({Seg::Join, -1, uint32_t(kJoinWindowUs), "join"});
  if (reserve) c.segs.push_back({Seg::Reserve, -1, uint32_t(d.reserveUs), d.satMask == 1 ? "R1" : d.satMask == 2 ? "R2" : "R"});
  uint32_t sum = 0;
  for (const auto& s : c.segs) sum += s.us;
  if (d.cycleUs > sum) c.segs.push_back({Seg::Rest, -1, d.cycleUs - sum, ""});
  c.total = std::max(sum, d.cycleUs);
  return c;
}

// Diagonal stripes over a rect (the repeater reserve).
void Stripes(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& c, float fill, float line) {
  dl->AddRectFilled(a, b, U32(c, fill));
  Hatch(dl, a, b, U32(c, line), 5 * Px(), 1.2f * Px());
}

// A drawn tile: a big value and what the value is.
void TileAt(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* value, const char* label, const ImVec4& color) {
  const float fs = ImGui::GetFontSize();
  Card(dl, a, b, kOff, 0.35f);
  ImGui::PushFont(g_huge);
  const float big = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  TextAt(dl, ImVec2(a.x + fs * 0.7f, a.y + fs * 0.5f), color, value, g_huge);
  TextAt(dl, ImVec2(a.x + fs * 0.7f, a.y + fs * 0.5f + big + fs * 0.1f), kMuted, label);
}

// Dims a drawn zone (its data is stale).
void DimZone(ImDrawList* dl, ImVec2 a, ImVec2 b) { dl->AddRectFilled(a, b, IM_COL32(17, 19, 25, 150)); }

// The info texts. Mode names come from ModeLabel.
const Info kInfoPinText = {
    {kAccent, "Pinned:", "the app sets this mode again when this dongle connects or restarts."},
    {kText, "Not pinned:", "the dongle starts in mode 3."},
    kNone,
    {kText, nullptr, "The pin is saved on this PC for this dongle."},
    {kText, nullptr, "A mode set later by another tool stays."}};
const Info kInfoCycleText = {
    {kText, nullptr, "The bar is one cycle, to scale. A shorter cycle means more updates per second."},
    kNone,
    {kText, nullptr, "Each numbered block is one device's turn. The warm fill is the average exchange. The thin mark is "
                     "the longest exchange. Dark room in a block is unused time."},
    {kOff, "join", "is the time kept free for new devices."},
    {kAccent, "R1, R2", "is the time kept for repeaters."},
    kNone,
    {kText, "live", "off: the page stops reading the dongle's timing counters. Switch live off while another tool "
                    "measures. Reading the counters resets the counters for every tool."}};
const Info kInfoTimingDevicesText = {
    {kText, "loss", "is the share of calls without an answer over the last second. Compare loss between modes. Hover "
                    "a loss bar for each mode's average this session."}};
}  // namespace

void Ui::Timing(const DongleState& d) {
  Title("Timing", Icon::Timing);
  if (!d.extras) {   // the nav item is hidden; this shows after switching dongles here
    Loadout(d, true);
    if (NeedsFirmware("timing modes")) page_ = Page::Firmware;
    return;
  }
  if (d.extras && !d.haveRole) {   // the role decides what this page is: wait for it
    ImGui::Spacing();
    WaitRow(now_);
    return;
  }
  if (d.haveRole && d.role == 1) { TimingForRepeater(d); return; }
  // live: 0x2D is read only while this is called
  Loadout(d, false);
  LiveCycle(d);
  TimingDevices(d);
}

// Zone A: a card per mode, like a loadout select: 3 (the default) and 1 (upstream's behaviour, kept for
// troubleshooting). Click a card to switch; the pin keeps the mode. Any other mode (set by another tool, such
// as `load.py timing 0`) shows as a notice under the cards, with the reset. oldFw: no timing modes (every card
// dimmed, nothing clickable).
void Ui::Loadout(const DongleState& d, bool oldFw) {
  const auto dongle = Current();
  SectionExtra legend;
  legend.body = GlyphLegend;
  Section("LOADOUT", infoOpen_, kInfoLoadout,
          {{kText, nullptr, "Many times a second, the dongle calls each device and the device answers with its tracking "
                             "data. A timing mode sets how the dongle schedules those calls."},
           kNone,
           {kGood, ModeLabel(3), "calls each device in turn. Each device's turn grows or shrinks with how much data the "
                                 "device sends. A hardware timer in the dongle starts every call at the exact planned "
                                 "moment, even while the dongle is busy talking to the PC over USB. On-time calls keep "
                                 "the devices in step and leave no dead air between turns, so the dongle gets more "
                                 "updates through each second. Use mode 3 for everyday play."},
           kNone,
           {kText, ModeLabel(1), "makes the same calls as mode 3, but the dongle's software starts each call. When the "
                                 "dongle is busy with USB, a call can go out a little late. Mode 1 is the original "
                                 "openhouse timing. Use mode 1 to rule out the timing when you troubleshoot."},
           kNone,
           {kText, nullptr, "Click a card to switch. Devices stay connected."},
           {kText, nullptr, "The dongle starts in mode 3 after every restart. Pin a card to keep a different mode."}},
          nullptr, &legend);
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), fh = ImGui::GetFrameHeight(), px = Px(), tlh = TitleHeight();
  const float W = ImGui::GetContentRegionAvail().x, pad = fs * 0.6f, gap = fs * 0.8f;
  constexpr int modes[] = {3, 1};
  const float cw = std::min(fs * 20, (W - gap) / 2);
  const float glyphH = lh * 1.6f, ch = pad * 2 + tlh + lh * 2 + fs * 0.5f + glyphH + fs * 0.4f + fh;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const auto keepIt = keepModes_.find(d.serial);
  // a saved pin on a mode this page doesn't offer is ignored (treated as no pin)
  const int keep = keepIt != keepModes_.end() && TimingModeOffered(keepIt->second) ? keepIt->second : -1;
  for (size_t k = 0; k < 2; ++k) {
    const int m = modes[k];
    const ImVec2 a(p.x + float(k) * (cw + gap), p.y), b(a.x + cw, a.y + ch);
    const CardState st = oldFw ? CardState::Locked : ModeCardState(d, m);
    const bool clickable = !oldFw && (st == CardState::Idle || st == CardState::Leaving || st == CardState::Locked);
    ImGui::PushID(m);
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("card", ImVec2(cw, ch));
    const bool hover = ImGui::IsItemHovered() && clickable;
    if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (clicked && clickable) {
      if (st == CardState::Locked) {
        page_ = Page::Firmware;
      } else {
        dongle->SetTimingMode(m, Report("Mode " + std::to_string(m)));
        if (keep >= 0) { keepModes_[d.serial] = m; knownDirty_ = true; }   // the pin follows the selection
      }
    }
    if (st == CardState::Locked && hover) ImGui::SetTooltip("This dongle firmware has no timing modes");
    // the card's edge carries the state
    const float alpha = st == CardState::Locked || st == CardState::Reading ? 0.45f : 1;
    if (oldFw) {
      dl->AddRectFilled(a, b, U32(lh::ui::CardColor()), fs * 0.5f);
      DashedRect(dl, a, b, U32(kOff, 0.5f), px);
    } else if (st == CardState::Running) {
      Card(dl, a, b, kGood, 1, 2);
      dl->AddRectFilled(a, b, U32(kGood, 0.06f), fs * 0.5f);
    } else if (st == CardState::Switching) {
      Card(dl, a, b, kInfo, 0.45f + 0.55f * float(0.5 + 0.5 * std::sin(now_ * 5)), 2);
    } else if (st == CardState::Leaving) {
      Card(dl, a, b, kGood, 0.5f, 2);
    } else {
      Card(dl, a, b, hover ? kAccent : kOff, hover ? 0.9f : st == CardState::Idle ? 0.45f : 0.3f, hover ? 1.5f : 1);
    }
    // number (title font) and badge; the word under them
    float y = a.y + pad;
    char num[4];
    std::snprintf(num, sizeof num, "%d", m);
    TextAt(dl, ImVec2(a.x + pad, y), st == CardState::Running ? kGood : kText, num, g_title, alpha);
    if (const char* tag = ModeTag(m)) {
      const ImVec2 bs = BadgeSize(tag);
      Badge(dl, ImVec2(b.x - pad - bs.x, y + (tlh - bs.y) / 2), tag, kGood, alpha);
    }
    y += tlh;
    TextAt(dl, ImVec2(a.x + pad, y), kText, ModeWord(m), nullptr, alpha);
    y += lh;
    TextAt(dl, ImVec2(a.x + pad, y), kMuted, ModeUse(m), nullptr, alpha);
    y += lh + fs * 0.5f;
    CycleGlyph(dl, ImVec2(a.x + pad, y), ImVec2(b.x - pad, y + glyphH), m, st == CardState::Running ? float(std::fmod(now_, 1.0)) : -1.f, alpha);
    // footer: the state at the left, the pin at the right
    const float cy = b.y - pad - fh / 2;
    if (!oldFw) {
      if (st == CardState::Reading) {
        WaitDots(dl, ImVec2(a.x + pad, cy), fs * 0.22f, kOff, now_);
      } else if (st == CardState::Locked) {
        UpdateChip(dl, a.x + pad, cy);
      } else if (st == CardState::Running || st == CardState::Leaving) {
        const float r = fs * 0.3f, la = st == CardState::Running ? 1.f : 0.5f;
        if (st == CardState::Running && std::fmod(now_, 1.0) > 0.1) dl->AddCircleFilled(ImVec2(a.x + pad + r, cy), r * 1.8f, U32(kGood, 0.2f), 16);
        dl->AddCircleFilled(ImVec2(a.x + pad + r, cy), r, U32(kGood, la), 16);
        TextAt(dl, ImVec2(a.x + pad + r * 2 + fs * 0.45f, cy - lh / 2), kGood, "ON", nullptr, la);
      } else if (st == CardState::Switching) {
        WaitDots(dl, ImVec2(a.x + pad, cy), fs * 0.18f, kInfo, now_);
        TextAt(dl, ImVec2(a.x + pad + fs * 1.7f, cy - lh / 2), kInfo, "SWITCHING");
      }
      const bool pinned = keep == m;
      if (pinned || (d.modeRequested == m && st != CardState::Locked)) {
        const float s = lh * 1.2f;
        if (PinGlyph("pin", ImVec2(b.x - pad - s, cy - s / 2), s, pinned, pinned ? kAccent : kMuted)) {
          if (pinned) { keepModes_.erase(d.serial); Toast("Pin removed", false, 3); }
          else { keepModes_[d.serial] = m; Toast("Mode " + std::to_string(m) + " pinned", false, 3); }
          knownDirty_ = true;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::PushTextWrapPos(fs * 26);
          InfoText(kInfoPinText);
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
      }
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, ch));
  if (oldFw || !d.haveTiming) return;
  // under the cards: a mode this app doesn't offer, in effect or requested, and the reset
  const int lab = !TimingModeOffered(d.modeNow) && d.modeNow >= 0 ? d.modeNow
                  : !TimingModeOffered(d.modeRequested) && d.modeRequested >= 0 ? d.modeRequested : -1;
  const bool reset = (d.modeRequested >= 0 && d.modeRequested != kDefaultTimingMode) || (keep >= 0 && keep != kDefaultTimingMode);
  if (lab < 0 && !reset) return;
  const ImVec2 q = ImGui::GetCursorScreenPos();
  if (lab >= 0) {
    TextAt(dl, q, kWarn, d.modeNow == lab ? "Custom timing, set by another tool." : "Custom timing requested by another tool.");
  }
  ImGui::SetCursorScreenPos(ImVec2(q.x, lab >= 0 ? q.y + lh + fs * 0.6f : q.y));
  if (reset) {
    char label[48];
    std::snprintf(label, sizeof label, "Reset to mode %d", kDefaultTimingMode);
    if (SmallTint(label, kAccent)) {
      const bool hadPin = keepModes_.erase(d.serial) > 0;
      if (hadPin) knownDirty_ = true;
      if (d.modeRequested != kDefaultTimingMode) {
        const std::string what = "Mode " + std::to_string(kDefaultTimingMode);
        dongle->SetTimingMode(kDefaultTimingMode, [this, hadPin, what](bool ok, const std::string& msg) {
          Post(ok ? msg + (hadPin ? ". Pin removed." : "") : what + " failed: " + msg, ok, 4);
        });
      } else if (hadPin) {
        Toast("Pin removed", false, 3);
      }
    }
  }
  if (!reset) ImGui::Dummy(ImVec2(W, 0));
}

// Zone B: the running cycle to scale and two tiles.
void Ui::LiveCycle(const DongleState& d) {
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), fh = ImGui::GetFrameHeight(), px = Px();
  Section("LIVE CYCLE", infoOpen_, kInfoCycle, kInfoCycleText);
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float W = ImGui::GetContentRegionAvail().x, gap = fs * 0.8f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool stale = !d.haveTiming || Now() - d.timingTime > 12;
  ImGui::PushFont(g_huge);
  const float big = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  const float tileW = fs * 7, tileH = big + lh + fs * 1.1f;
  char v[32];
  if (d.haveTiming && d.cycleUs) std::snprintf(v, sizeof v, "%.0f", 1e6 / d.cycleUs); else std::snprintf(v, sizeof v, "-");
  TileAt(dl, p, ImVec2(p.x + tileW, p.y + tileH), v, "cycles/s", kInfo);
  if (d.haveTiming && d.cycleUs) std::snprintf(v, sizeof v, "%.1f", d.cycleUs / 1000.0); else std::snprintf(v, sizeof v, "-");
  TileAt(dl, ImVec2(p.x + tileW + gap, p.y), ImVec2(p.x + tileW * 2 + gap, p.y + tileH), v, "ms", kText);
  // the bar: tick labels above, the parts, the legend under
  const float bx0 = p.x + tileW * 2 + gap * 2.5f, bx1 = p.x + W, barW = bx1 - bx0;
  const float by0 = p.y + lh + fs * 0.25f, barH = lh + fh * 0.75f, by1 = by0 + barH;
  const float zoneH = std::max(tileH, by1 - p.y + fs * 0.4f + lh);
  if (!d.haveTiming) {
    WaitDots(dl, ImVec2(bx0, by0 + barH / 2), fs * 0.3f, kOff, now_);
  } else {
    const Cycle cyc = BuildCycle(d);
    const float k = cyc.total ? barW / float(cyc.total) : 0;
    TextAt(dl, ImVec2(bx0, p.y), kMuted, "0");
    const std::string right = Us(d.cycleUs);
    const float rw = ImGui::CalcTextSize(right.c_str()).x;
    TextAt(dl, ImVec2(bx1 - rw, p.y), kMuted, right.c_str());
    char age[32];
    std::snprintf(age, sizeof age, "Read %.0f s ago", std::max(0.0, Now() - d.timingTime));
    AreaTip(ImVec2(bx1 - rw, p.y), ImVec2(bx1, p.y + lh), age);
    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), U32(kTrack), fs * 0.25f);
    float x = bx0;
    const float fillY0 = by0 + lh + fs * 0.05f;
    for (const auto& s : cyc.segs) {
      const float w = float(s.us) * k, x1 = x + w;
      const ImVec2 a(x, by0), b(x1, by1);
      std::string tip;
      char t[160];
      switch (s.kind) {
        case Seg::Device: {
          const float pollW = std::min(w * 0.12f, fs * 0.45f);
          const float ex = std::min(w, float(d.exAvgUs) * k), mx = std::min(w, float(d.exMaxUs) * k);
          dl->AddRectFilled(ImVec2(x, fillY0), ImVec2(x + std::max(ex, pollW), by1), U32(kAccent, 0.8f));
          dl->AddRectFilled(ImVec2(x, by0), ImVec2(x + pollW, by1), U32(kInfo, 0.95f));
          if (d.exMaxUs) dl->AddLine(ImVec2(x + mx, fillY0 - fs * 0.2f), ImVec2(x + mx, by1), U32(kText, 0.9f), 1.5f * px);
          std::snprintf(t, sizeof t, "%d  %s\nSlot width %s\nExchange %s\nLongest %s\nAnswer up to %d B", s.slot, DeviceLabel(d.slots[size_t(s.slot)]).c_str(),
                        Us(d.slotUs).c_str(), Us(d.exAvgUs).c_str(), Us(d.exMaxUs).c_str(), d.answerBytes);
          tip = t;
          break;
        }
        case Seg::Join:
          dl->AddRectFilled(ImVec2(x + px, by0 + px), ImVec2(x1 - px, by1 - px), U32(kOff, 0.3f));
          tip = "Join window " + Us(s.us);
          break;
        case Seg::Reserve:
          Stripes(dl, ImVec2(x + px, by0 + px), ImVec2(x1 - px, by1 - px), kAccent, 0.2f, 0.7f);
          std::snprintf(t, sizeof t, "%s  %.0f %%", Us(s.us).c_str(), d.cycleUs ? 100.0 * s.us / d.cycleUs : 0.0);
          tip = t;
          break;
        case Seg::Rest: break;
      }
      if (s.kind != Seg::Rest && x > bx0) dl->AddLine(ImVec2(x, by0), ImVec2(x, by1), U32(ImVec4(0.067f, 0.075f, 0.098f, 1)), 1.5f * px);
      if (!s.label.empty()) {
        const float tw = ImGui::CalcTextSize(s.label.c_str()).x;
        if (tw + fs * 0.4f < w)
          TextAt(dl, ImVec2(s.kind == Seg::Device ? x + std::min(w * 0.12f, fs * 0.45f) + fs * 0.25f : x + (w - tw) / 2, by0 + fs * 0.05f),
                 s.kind == Seg::Reserve ? kAccent : s.kind == Seg::Join ? kMuted : kText, s.label.c_str());
      }
      if (!tip.empty()) AreaTip(a, b, tip);
      x = x1;
    }
    // under the bar: the legend
    const float ly = by1 + fs * 0.4f;
    {
      const float sw = fs * 1.2f, sh = lh * 0.6f, sy = ly + (lh - sh) / 2;
      float lx = bx0;
      const bool reserve = d.haveRole && d.role == 0 && d.satMask && d.reserveUs > 0;
      for (int kk = 0; kk < 5; ++kk) {
        if (kk == 4 && !reserve) break;
        const char* words[] = {"dongle", "device answers", "longest", "new devices", "repeater"};
        const ImVec2 a(lx, sy), b(lx + sw, sy + sh);
        if (kk == 0) dl->AddRectFilled(ImVec2(a.x + sw * 0.35f, a.y), ImVec2(a.x + sw * 0.65f, b.y), U32(kInfo, 0.95f));
        else if (kk == 1) dl->AddRectFilled(a, b, U32(kAccent, 0.8f));
        else if (kk == 2) dl->AddLine(ImVec2(a.x + sw / 2, a.y - px), ImVec2(a.x + sw / 2, b.y + px), U32(kText, 0.9f), 1.5f * px);
        else if (kk == 3) dl->AddRectFilled(a, b, U32(kOff, 0.3f));
        else Stripes(dl, a, b, kAccent, 0.2f, 0.7f);
        TextAt(dl, ImVec2(b.x + fs * 0.35f, ly), kMuted, words[kk]);
        lx = b.x + fs * 0.35f + ImGui::CalcTextSize(words[kk]).x + fs * 1.2f;
      }
    }
    if (!stale) {   // the playhead: once a second (real cycles are far too fast to show)
      const float hx = bx0 + barW * float(std::fmod(now_, 1.0));
      dl->AddLine(ImVec2(hx, by0 - fs * 0.2f), ImVec2(hx, by1 + fs * 0.2f), U32(kText, 0.85f), 1.5f * px);
    }
  }
  const float zoneEnd = p.y + zoneH;
  if (stale && d.haveTiming) DimZone(dl, p, ImVec2(p.x + W, zoneEnd));
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, zoneEnd - p.y));
}

// Zone C: one row per paired device: signal and loss (hover a loss bar: each mode's average this session).
void Ui::TimingDevices(const DongleState& d) {
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), fh = ImGui::GetFrameHeight();
  Section("DEVICES", infoOpen_, kInfoTimingDevices, kInfoTimingDevicesText);
  ImGui::Spacing();
  if (d.paired == 0) {
    ImGui::TextDisabled("Nothing paired");
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float W = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float xSig = p.x + fs * 1.6f, xIcon = p.x + fs * 3.4f, xNum = xIcon + lh + fs * 0.4f, xName = xNum + fs * 1.4f;
  const float lossTextW = fs * 4, rest = W - (xName - p.x) - lossTextW - fs * 2;
  const float nameW = std::clamp(rest * 0.45f, fs * 8, fs * 16), lossW = std::clamp(rest - nameW, fs * 8, fs * 17);
  const float xBar = xName + nameW + fs * 0.5f, xLossT = xBar + lossW + fs * 0.6f, xEnd = xLossT + lossTextW;
  TextCentered(dl, ImVec2(xSig, p.y + lh / 2), kMuted, "signal");
  TextAt(dl, ImVec2(xBar, p.y), kMuted, "loss");
  const float rowY0 = p.y + lh + fs * 0.3f, rowH = fh;
  const auto meansIt = modeMeans_.find(d.serial);
  int n = 0;
  for (int i = 0; i < kSlots; ++i) {
    const auto& s = d.slots[i];
    if (!s.used) continue;
    const float y0 = rowY0 + float(n++) * rowH, cy = y0 + rowH / 2, dim = s.active ? 1.f : 0.45f;
    if (s.active) {
      SignalPill(dl, ImVec2(xSig, cy), s.rssiDongle, lh * 0.75f);
      char t[24];
      std::snprintf(t, sizeof t, "%d dBm", s.rssiDongle);
      AreaTip(ImVec2(p.x, y0), ImVec2(xIcon - fs * 0.2f, y0 + rowH), t);
    }
    lh::ui::DrawIcon(dl, Icon::Tracker, ImVec2(xIcon + lh / 2, cy), lh, U32(s.active ? kText : kOff, s.active ? 0.85f : 0.6f));
    char num[8];
    std::snprintf(num, sizeof num, "%d", i);
    TextAt(dl, ImVec2(xNum, cy - lh / 2), kMuted, num, nullptr, dim);
    const std::string name = DeviceLabel(s);
    dl->PushClipRect(ImVec2(xName, y0), ImVec2(xName + nameW, y0 + rowH), true);
    TextAt(dl, ImVec2(xName, cy - lh / 2), s.active ? kText : kOff, name.c_str());
    dl->PopClipRect();
    if (ImGui::CalcTextSize(name.c_str()).x > nameW) AreaTip(ImVec2(xName, y0), ImVec2(xName + nameW, y0 + rowH), name);
    const ImVec2 ba(xBar, cy - lh * 0.38f), bb(xBar + lossW, cy + lh * 0.38f);
    if (!s.active) {
      dl->AddRectFilled(ba, bb, U32(kTrack, 0.6f), (bb.y - ba.y) * 0.3f);
      TextAt(dl, ImVec2(ba.x + fs * 0.5f, cy - lh / 2), kOff, "off");
      continue;
    }
    LossBar(dl, ba, bb, s.lossPercent, s.lossPercent);
    if (s.lossPercent >= 0) {
      char t[16];
      std::snprintf(t, sizeof t, "%.0f %%", s.lossPercent);
      TextAt(dl, ImVec2(xLossT, cy - lh / 2), LossColor(s.lossPercent), t);
      // the mode scoreboard: this session's average loss per mode in effect
      std::string tip;
      char t2[64];
      std::snprintf(t2, sizeof t2, "Loss %.1f %%", s.lossPercent);
      tip = t2;
      if (meansIt != modeMeans_.end())
        for (int m : {3, 1})
          if (meansIt->second.n[m][size_t(i)] >= 5) {
            std::snprintf(t2, sizeof t2, "\nMode %d average %.1f %%  (%d s)", m, meansIt->second.sum[m][size_t(i)] / meansIt->second.n[m][size_t(i)],
                          meansIt->second.n[m][size_t(i)]);
            tip += t2;
          }
      AreaTip(ImVec2(xBar, y0), ImVec2(xEnd, y0 + rowH), tip);
    }
  }
  const float zoneEnd = rowY0 + float(n) * rowH;
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, zoneEnd - p.y));
}

// A dongle in the repeater role follows the main's cycle: the timing mode is set on the main. The page shows
// where the timing lives and the jump to the main.
void Ui::TimingForRepeater(const DongleState& d) {
  const auto& all = manager_.Dongles();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight(), px = Px(), tlh = TitleHeight();
  ImGui::Spacing();
  ImGui::Spacing();
  if (d.roleRunning != d.role) {   // the role change hasn't taken effect yet
    WaitRow(now_);
    return;
  }
  const auto main = FindByNetBase(all, d.mainBase, d.serial);
  const DongleState m = main ? main->Snapshot() : DongleState{};
  const float W = ImGui::GetContentRegionAvail().x, bw = fs * 11, bh = fs * 2.6f + tlh + lh * 2 + fs * 0.8f, gap = fs * 16;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float x0 = p.x + std::max(0.f, (W - bw * 2 - gap) / 2), x1 = x0 + bw + gap;
  NodeCard(dl, ImVec2(x0, p.y), ImVec2(x0 + bw, p.y + bh), kAccent, "REPEATER", RName(d.satId) + "  " + Short4(d.serial));
  NodeCard(dl, ImVec2(x1, p.y), ImVec2(x1 + bw, p.y + bh), kInfo, "MAIN", Short4(m.serial), nullptr,
           m.haveTiming ? ModeLabel(m.modeNow) : nullptr, main ? nullptr : "Main unplugged");
  // the arrow: the repeater follows the main's cycle
  const float ay = p.y + bh / 2, ax0 = x0 + bw + fs * 0.8f, ax1 = x1 - fs * 0.8f;
  const ImVec4 ac = main ? kAccent : kOff;
  if (main) dl->AddLine(ImVec2(ax0, ay), ImVec2(ax1 - fs * 0.6f, ay), U32(ac), 3 * px);
  else Line(dl, ImVec2(ax0, ay), ImVec2(ax1 - fs * 0.6f, ay), U32(ac, 0.6f), 2 * px, Stroke::Dashed);
  Arrowhead(dl, ImVec2(ax1, ay), 1, 0, fs * 0.8f, U32(ac, main ? 1.f : 0.6f));
  TextCentered(dl, ImVec2((ax0 + ax1) / 2, ay - fs * 1.1f), kMuted, "follows the cycle");
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, bh));
  ImGui::Spacing();
  ImGui::Spacing();
  if (main) {
    const float bw2 = fs * 13;
    ImGui::SetCursorScreenPos(ImVec2(x0 + (bw * 2 + gap - bw2) / 2, ImGui::GetCursorScreenPos().y));
    if (TintButton("Show main's timing", kInfo, bw2)) dongle_ = m.serial;
  } else {
    const char* t = "Timing is set on the main.";
    ImGui::SetCursorScreenPos(ImVec2(x0 + (bw * 2 + gap - ImGui::CalcTextSize(t).x) / 2, ImGui::GetCursorScreenPos().y));
    ImGui::TextColored(kMuted, "%s", t);
  }
}

void Ui::Firmware(const DongleState& d) {
  Title("Firmware", lh::ui::Icon::Chip);
  const auto dongle = Current();
  if (ImGui::BeginTable("fw", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    const auto row = [](const char* a, const std::string& b) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::TextDisabled("%s", a);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
    };
    char t[96];
    row("Dongle", d.serial);
    std::snprintf(t, sizeof t, "%d (%s build%s)", d.firmware,
                  d.board == 4 ? "Valve Index radio" : d.board == 1 ? "Holyiot 21017" : d.board == 2 ? "PCA10059" : d.board == 3 ? (d.bank == 2 ? "Kuroshin, bank B" : "Kuroshin, bank A") : d.softdeviceVariant ? "Nordic bootloader" : "bare",
                  d.watchdog ? ", watchdog" : "");
    row("Firmware", t);
    row("Up for", Uptime(d.uptimeMs));
    std::snprintf(t, sizeof t, "0x%X", d.resetReason);
    row("Last reset", d.resetReason == 0 ? "power on" : d.resetReason & 1 ? "reset pin" : d.resetReason & 4 ? "software" : d.resetReason & 2 ? "watchdog" : t);
    std::snprintf(t, sizeof t, "%u%s", d.joinsHeard, "");
    row("Join requests heard", t);
    if (d.crashCount) {
      std::snprintf(t, sizeof t, "%u (last: type %u at 0x%08X)", d.crashCount, d.crashType, d.crashPc);
      row("Crashes", t);
    } else {
      row("Crashes", "none");
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  if (BigButton("Restart dongle", ImGui::GetFontSize() * 12)) ImGui::OpenPopup("Restart?");
  if (d.crashCount) {
    ImGui::SameLine();
    if (BigButton("Clear crash record", ImGui::GetFontSize() * 12)) dongle->ClearCrash(Report("Clear crash record"));
  }
  if (Confirm("Restart?", "Every device drops for a moment")) dongle->Reset(Report("Restart"));

  ImGui::Spacing();
  ImGui::SeparatorText("Update");
  if (d.IndexRadio()) {
    ImGui::TextWrapped("Update it from openhouse-installer: Update Dongles");
  } else if (d.Banked()) {
    // Updates itself: the packaged firmware for the bank that isn't running.
    const fs::path image = PackagedBankImage(lh::dongle_bank::ImageName(d.board, d.softdeviceVariant, d.bank));
    if (image.empty()) ImGui::TextColored(kWarn, "Firmware not found next to the app");
    else ImGui::TextDisabled("%s", lh::platform::PathUtf8(image.filename()).c_str());
    ImGui::BeginDisabled(image.empty() || updating_);
    if (TintButton(updating_ ? "Updating..." : "Update", kAccent, ImGui::GetFontSize() * 12)) ImGui::OpenPopup("Update?");
    ImGui::EndDisabled();
    if (Confirm("Update?", "Every device drops for a few seconds")) StartBankUpdate(dongle, image);
  } else {
    // Older firmware: through its bootloader, from a file.
    if (d.NordicDfu())
      ImGui::TextWrapped("Pick %s", d.board == 2 ? "doorman-pca10059.hex" : "doorman-holyiot.hex");
    else
      ImGui::TextWrapped("Pick %s", d.softdeviceVariant ? "doorman-dongle-sd.uf2" : "doorman-dongle-nosd.uf2");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13);
    ImGui::InputTextWithHint("##uf2", d.NordicDfu() ? "path to the .hex file" : "path to the .uf2 file", firmwarePath_, sizeof firmwarePath_);
    ImGui::SameLine();
    const bool exists = firmwarePath_[0] && fs::exists(fs::u8path(firmwarePath_));
    ImGui::BeginDisabled(!exists || updating_);
    if (TintButton(updating_ ? "Updating..." : "Update", kAccent, -1)) ImGui::OpenPopup("Update?");
    ImGui::EndDisabled();
    if (Confirm("Update?", "Every device drops for about 10 s")) StartUpdate(dongle, firmwarePath_, d.NordicDfu());
  }
  {
    std::lock_guard lock(updateMutex_);
    if (!updateStatus_.empty()) ImGui::TextColored(updateError_ ? kBad : updating_ ? kInfo : kGood, "%s", updateStatus_.c_str());
  }
}

// The package's firmwares/dongle/bank/<name>: this app runs from the package's apps/, or its root.
fs::path Ui::PackagedBankImage(const std::string& name) {
  std::error_code ec;
#ifdef _WIN32
  wchar_t self[MAX_PATH * 2]{};
  GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  const fs::path dir = fs::path(self).parent_path();
#else
  fs::path dir = fs::read_symlink("/proc/self/exe", ec).parent_path();
  if (ec) dir = fs::current_path();
#endif
  for (const fs::path& base : {dir, dir.parent_path()}) {
    const fs::path p = base / "firmwares" / "dongle" / "bank" / name;
    if (fs::exists(p, ec)) return p;
  }
  return {};
}

// Updates itself: the image for the bank that isn't running goes over USB (lh/dongle_bank.hpp).
void Ui::StartBankUpdate(std::shared_ptr<Dongle> dongle, const fs::path& file) {
  if (updating_) return;
  if (updater_.joinable()) updater_.join();
  const auto set = [this](const std::string& s, bool error = false) {
    std::lock_guard lock(updateMutex_);
    updateStatus_ = s;
    updateError_ = error;
  };
  std::ifstream in(file, std::ios::binary);
  std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (image.size() < 1024) { set("Can't read " + lh::platform::PathUtf8(file), true); return; }
  updating_ = true;
  set("Writing the firmware...");
  dongle->BankUpdate(std::move(image),
                     [set](float f) { char t[48]; std::snprintf(t, sizeof t, "Writing the firmware... %d%%", int(f * 100)); set(t); },
                     [this, set](bool ok, const std::string& message) { set(ok ? message : "Update failed: " + message, !ok); updating_ = false; });
}

void Ui::StartUpdate(std::shared_ptr<Dongle> dongle, const std::string& file, bool nordicDfu) {
  if (updating_) return;
  if (updater_.joinable()) updater_.join();
  updating_ = true;
  const auto set = [this](const std::string& s, bool error = false) {
    std::lock_guard lock(updateMutex_);
    updateStatus_ = s;
    updateError_ = error;
  };
  set("Putting the dongle in update mode...");
  if (nordicDfu) {
    updater_ = std::thread([this, dongle, file, set] {
      lh::dfu::Image image;
      std::string error;
      if (!lh::dfu::LoadHex(file, 0x1000, 0xDA000, &image, &error) || image.base != 0x1000) {
        set(error.empty() ? "This file isn't built for this dongle." : "Can't use this file: " + error, true);
        updating_ = false;
        return;
      }
      const auto before = lh::dfu::FindBootloaderPorts();
      dongle->EnterBootloader(nullptr);
      std::string port;
      for (const double until = Now() + 20; Now() < until && port.empty();) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (const auto& p : lh::dfu::FindBootloaderPorts())
          if (std::find(before.begin(), before.end(), p) == before.end()) port = p;
      }
      if (port.empty()) { set("The DFU bootloader didn't show up. Press the dongle's RESET button and try again.", true); updating_ = false; return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the port settle
      const bool ok = lh::dfu::Flash(port, image, [&](float f) {
        char text[64];
        std::snprintf(text, sizeof text, "Sending firmware... %d%%", int(f * 100));
        set(text);
      }, &error);
      set(ok ? "Done: the dongle restarts with the new firmware." : "Update failed: " + error, !ok);
      updating_ = false;
    });
    return;
  }
  updater_ = std::thread([this, dongle, file, set] {
    const auto before = Uf2Drives();
    dongle->EnterBootloader(nullptr);
    const double until = Now() + 20;
    fs::path drive;
    while (Now() < until && drive.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      for (const auto& d : Uf2Drives())
        if (std::find(before.begin(), before.end(), d) == before.end() || before.size() == 0) drive = d;
    }
    if (drive.empty()) {
      set("The update drive didn't show up. If it appeared but isn't mounted, mount it and copy the file onto it yourself.", true);
      updating_ = false;
      return;
    }
    set("Copying onto " + lh::platform::PathUtf8(drive) + "...");
    std::error_code ec;
    fs::copy_file(fs::u8path(file), drive / fs::u8path(file).filename(), fs::copy_options::overwrite_existing, ec);
    std::error_code gone;
    if (ec && fs::exists(drive, gone)) {   // the drive leaves as the dongle restarts: that's success
      set("Copy failed: " + ec.message(), true);
    } else {
      set("Done: the dongle restarts with the new firmware.");
    }
    updating_ = false;
  });
}

void Ui::About() {
  Title("About", lh::ui::Icon::About);
  ImGui::TextWrapped("Doorman Dongle Manager");
  ImGui::Spacing();
  ImGui::TextWrapped("Eight slots, one device each. SteamVR's pairing page works too while the openhouse driver runs");
  ImGui::Spacing();
  ImGui::TextDisabled("openhouse %s", lh::kVersion);
  if (ImGui::Button("Release page")) overlay::UpdateCheck::OpenReleasesPage();
}

}  // namespace openhouse::dongle
