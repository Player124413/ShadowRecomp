// See log_ring.h.
#include "log_ring.h"

#include "gx_lle.h"
#include "settings_ui.h"
#include "sys.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace log_ring {
namespace {

constexpr size_t kKeep = 400;   // lines in the ring
constexpr size_t kMaxLine = 400;   // a longer line is cut (a vertex dump would otherwise fill the ring)
constexpr size_t kShown = 16;      // lines in the overlay: what fits above the touch controls
constexpr size_t kErrorsKept = 8;  // error lines kept for the top of the overlay

std::mutex g_mutex;
std::deque<std::string> g_lines;
std::deque<std::string> g_errors;
unsigned long long g_total = 0;
unsigned long long g_seen_total = 0;   // what the overlay last rebuilt its text from

// The overlay's text, rebuilt only when something new arrived: ImGui text is laid out every frame, and re-joining 16
// strings per frame for a debug overlay is a waste of the frame time it exists to measure.
std::string g_text;
std::string g_error_text;

// Case-insensitive "does this line report a failure?" - Aurora/Dawn/Tint word their messages differently, but every
// one of them that matters says one of these words.
bool looks_like_error(const std::string& line) {
  static const char* kWords[] = {"error", "fatal", "failed", "failure", "invalid", "unsupported", "not supported",
                                 "validation"};
  std::string lower;
  lower.reserve(line.size());
  for (char c : line) lower += (char)tolower((unsigned char)c);
  for (const char* word : kWords) {
    if (lower.find(word) != std::string::npos) return true;
  }
  return false;
}

bool env_forces() {
  const char* e = getenv("SHADOW_LOG");
  return e != nullptr && *e != '0' && *e != 'f' && *e != 'F';
}

}  // namespace

void add(const char* text, size_t len) {
  if (text == nullptr || len == 0) return;
  // one line per entry: split on newlines so the ring does not hold a whole block of text as one entry
  size_t start = 0;
  while (start < len) {
    size_t end = start;
    while (end < len && text[end] != '\n') ++end;
    size_t n = end - start;
    if (n > kMaxLine) n = kMaxLine;
    if (n != 0) {
      std::lock_guard lk(g_mutex);
      g_lines.emplace_back(text + start, n);
      while (g_lines.size() > kKeep) g_lines.pop_front();
      ++g_total;
      if (looks_like_error(g_lines.back()) && (g_errors.empty() || g_errors.back() != g_lines.back())) {
        g_errors.push_back(g_lines.back());   // same message twice in a row is one message (a per-frame error)
        while (g_errors.size() > kErrorsKept) g_errors.pop_front();
        g_seen_total = ~0ull;                 // force the overlay to rebuild its text
      }
    }
    start = end + 1;
  }
  // A line that was cut says so, so a truncated message is never mistaken for the whole one.
  if (len > 0 && text[len - 1] != '\n') {
    std::lock_guard lk(g_mutex);
    if (!g_lines.empty() && g_lines.back().size() >= kMaxLine) g_lines.back() += " ...";
  }
}

std::vector<std::string> lines(size_t count) {
  std::lock_guard lk(g_mutex);
  std::vector<std::string> out;
  const size_t n = count < g_lines.size() ? count : g_lines.size();
  out.reserve(n);
  for (size_t i = g_lines.size() - n; i < g_lines.size(); ++i) out.push_back(g_lines[i]);
  return out;
}

std::vector<std::string> errors(size_t count) {
  std::lock_guard lk(g_mutex);
  std::vector<std::string> out;
  const size_t n = count < g_errors.size() ? count : g_errors.size();
  out.reserve(n);
  for (size_t i = g_errors.size() - n; i < g_errors.size(); ++i) out.push_back(g_errors[i]);
  return out;
}

unsigned long long total() {
  std::lock_guard lk(g_mutex);
  return g_total;
}

bool overlay_wanted() { return env_forces() || settings::g.show_log; }

void draw_overlay() {
  if (!overlay_wanted()) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float w = vp->Size.x, h = vp->Size.y;
  const float scale = std::max(1.0f, h / 720.0f);   // readable on a phone panel and on a desktop window

  const unsigned long long total_now = total();
  if (g_seen_total != total_now || g_text.empty()) {
    g_seen_total = total_now;
    const auto tail = lines(kShown);
    g_text.clear();
    for (const auto& l : tail) {
      g_text += l;
      g_text += '\n';
    }
    g_error_text.clear();
    for (const auto& l : errors(kErrorsKept)) {
      g_error_text += l;
      g_error_text += '\n';
    }
  }

  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 4 * scale, vp->Pos.y + h * 0.62f));
  ImGui::SetNextWindowSize(ImVec2(w * 0.98f, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * scale, 4 * scale));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 190));
  ImGui::Begin("##log_overlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
  if (!g_error_text.empty()) {
    // red first: this is the part that names the bug
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 120, 255));
    ImGui::TextUnformatted(g_error_text.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
  }
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(225, 235, 250, 255));
  ImGui::TextUnformatted(g_text.c_str());
  ImGui::PopStyleColor();
  ImGui::PopStyleColor();
  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace log_ring
