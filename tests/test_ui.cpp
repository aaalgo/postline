#include <cstdlib> // atexit
#include <string>
#include <filesystem>
#include <iostream>
#include <termcolor/termcolor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/box.hpp>
#include <postline/ui/rich_log_view.hpp>

using namespace ftxui;
//using namespace postline;

namespace {

bool g_follow_tail = true;

struct AppState {
  std::vector<std::string> tab1_left_lines;
};

std::vector<std::string> SplitLines(const std::string& content) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= content.size()) {
    const size_t end = content.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(content.substr(start));
      break;
    }
    lines.push_back(content.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

bool IsSubmitShortcut(Event event) {
  return event == Event::F12 ||
         event == Event::CtrlAltM ||
         event.input() == "\x1B\r" ||
         event.input() == "\x1B\n" ||
         event.input() == "\x1B[27;3;13~" ||
         event.input() == "\x1B[13;3u";
}

std::vector<std::string> MakeInitialTab1OutputLines() {
  constexpr const char* kSubjects[] = {
      "worker", "queue", "cache", "stream", "logger", "router",
  };
  constexpr const char* kVerbs[] = {
      "synced", "flushed", "scheduled", "merged", "parsed", "replayed",
  };
  constexpr const char* kObjects[] = {
      "delta", "snapshot", "payload", "segment", "cursor", "checkpoint",
  };
  constexpr size_t kSubjectCount = sizeof(kSubjects) / sizeof(kSubjects[0]);
  constexpr size_t kVerbCount = sizeof(kVerbs) / sizeof(kVerbs[0]);
  constexpr size_t kObjectCount = sizeof(kObjects) / sizeof(kObjects[0]);

  uint32_t state = 0xC0FFEEu;
  const auto next = [&] {
    state = state * 1664525u + 1013904223u;
    return state;
  };

  std::vector<std::string> output;
  output.reserve(50);
  for (int i = 0; i < 50; ++i) {
    std::string line;
    line += "[";
    line += std::to_string(i + 1);
    line += "] ";
    line += kSubjects[next() % kSubjectCount];
    line += " ";
    line += kVerbs[next() % kVerbCount];
    line += " ";
    line += kObjects[next() % kObjectCount];
    line += " #";
    line += std::to_string(next() % 1000);
    output.push_back(std::move(line));
  }
  return output;
}

std::vector<std::string> MakeRandomTab1LeftLines(uint32_t* seed) {
  constexpr const char* kAdjectives[] = {
      "quiet", "brisk", "sharp", "steady", "dense", "rapid",
  };
  constexpr const char* kNouns[] = {
      "alpha", "buffer", "cursor", "packet", "worker", "stream",
  };
  constexpr const char* kStates[] = {
      "ready", "queued", "active", "paused", "draining", "settled",
  };
  constexpr size_t kAdjectiveCount =
      sizeof(kAdjectives) / sizeof(kAdjectives[0]);
  constexpr size_t kNounCount = sizeof(kNouns) / sizeof(kNouns[0]);
  constexpr size_t kStateCount = sizeof(kStates) / sizeof(kStates[0]);

  const auto next = [&] {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
  };

  std::vector<std::string> lines;
  lines.reserve(12);
  for (int i = 0; i < 12; ++i) {
    std::string line;
    line += kAdjectives[next() % kAdjectiveCount];
    line += " ";
    line += kNouns[next() % kNounCount];
    line += " ";
    line += kStates[next() % kStateCount];
    line += " #";
    line += std::to_string(next() % 100);
    lines.push_back(std::move(line));
  }
  return lines;
}

Element MakeTab1WelcomeOverlay() {
  return vbox({
             filler(),
             text("welcome to aaalgo") | hcenter | color(Color::GrayLight),
             text("postline") | hcenter | bold | color(Color::CyanLight),
             /*
             text(std::string("version ") + postline::build::VERSION +
                  " | build " + postline::build::BUILD_TYPE + " | commit " +
                  postline::build::GIT_COMMIT) |
                 hcenter | color(Color::GrayDark),
                 */
             filler(),
         }) |
         flex;
}

}  // namespace

int main(int argc, char** argv) {
  // Parse CLI options

    /*
  init_logging();
  setup_environ();
  */

  auto screen = ScreenInteractive::Fullscreen();

  uint32_t random_seed = 0x1234ABCDu;
  AppState app_state = {
      .tab1_left_lines = MakeRandomTab1LeftLines(&random_seed),
  };
  StringLogData tab1_log_data(2000);
  tab1_log_data.SetOnChange([&screen] { screen.PostEvent(Event::Custom); });
  for (const auto& line : MakeInitialTab1OutputLines()) {
    tab1_log_data.append(line);
  }
  StringLogData tab3_log_data(200);
  int tab3_scroll = 0;
  tab3_log_data.append("");
  tab3_log_data.append(
      "██████╗  ██████╗ ███████╗████████╗██╗     ██╗███╗   ██╗███████╗",
      Color::GreenLight);
  tab3_log_data.append(
      "██╔══██╗██╔═══██╗██╔════╝╚══██╔══╝██║     ██║████╗  ██║██╔════╝",
      Color::GreenLight);
  tab3_log_data.append(
      "██████╔╝██║   ██║███████╗   ██║   ██║     ██║██╔██╗ ██║█████╗",
      Color::GreenLight);
  tab3_log_data.append(
      "██╔═══╝ ██║   ██║╚════██║   ██║   ██║     ██║██║╚██╗██║██╔══╝",
      Color::GreenLight);
  tab3_log_data.append(
      "██║     ╚██████╔╝███████║   ██║   ███████╗██║██║ ╚████║███████╗",
      Color::GreenLight);
  tab3_log_data.append(
      "╚═╝      ╚═════╝ ╚══════╝   ╚═╝   ╚══════╝╚═╝╚═╝  ╚═══╝╚══════╝",
      Color::GreenLight);
  tab3_log_data.append("");
  tab3_log_data.append("Postline Agent Runtime", Color::CyanLight);
  tab3_log_data.append("By Ann Arbor Algorithms", Color::CyanLight);
  /*
  tab3_log_data.append("");
  tab3_log_data.append("  Version: " + std::string(postline::build::VERSION));
  tab3_log_data.append("  Build:   " + std::string(postline::build::BUILD_TYPE));
  tab3_log_data.append("  Commit:  " + std::string(postline::build::GIT_COMMIT));
  */
  std::string tab1_input_content;
  int tab1_top_scroll = std::numeric_limits<int>::max();
  Box tab1_bottom_box;
  auto tab1_input_option = InputOption();
  tab1_input_option.multiline = true;
  tab1_input_option.transform = [](InputState state) {
    if (state.is_placeholder) {
      state.element |= dim;
    }
    if (!state.focused) {
      state.element |= color(Color::GrayDark);
    }
    return state.element;
  };
  auto tab1_input =
      Input(&tab1_input_content, "Write here...", tab1_input_option);

  auto append_tab1_input = [&] {
    if (tab1_input_content.empty()) {
      return;
    }
    auto appended_lines = SplitLines(tab1_input_content);
    for (const auto& line : appended_lines) {
      tab1_log_data.append(line);
    }
    tab1_input_content.clear();
    if (g_follow_tail) {
      tab1_top_scroll = std::numeric_limits<int>::max();
    }
  };

  auto tab1_top = RichLogView({
      .data = &tab1_log_data,
      .scroll = &tab1_top_scroll,
      .overscan = 1,
  });
  auto tab1_bottom = Renderer(tab1_input, [&] {
    Element input = tab1_input->Render() | frame;
    if (!tab1_input_content.empty()) {
      return input | reflect(tab1_bottom_box);
    }
    return dbox({
               input,
               MakeTab1WelcomeOverlay(),
           }) |
           reflect(tab1_bottom_box);
  });
  tab1_bottom = CatchEvent(tab1_bottom, [&](Event event) {
    if (event == Event::PageUp || event == Event::PageDown) {
      return true;
    }
    if (!event.is_mouse()) {
      return false;
    }
    if (!tab1_bottom_box.Contain(event.mouse().x, event.mouse().y)) {
      return false;
    }
    return event.mouse().button == Mouse::WheelUp ||
           event.mouse().button == Mouse::WheelDown;
  });
  auto focus_tab1_input = [&] {
    tab1_input->TakeFocus();
  };

  int tab1_bottom_size = 4;
  auto tab1_right =
      ResizableSplitBottom(tab1_bottom, tab1_top, &tab1_bottom_size);

  auto tab1_left = Renderer([&](bool focused) {
    Elements entries;
    for (const auto& entry : app_state.tab1_left_lines) {
      entries.push_back(text(entry));
    }
    if (entries.empty()) {
      entries.push_back(text(""));
    }
    Element pane = vbox(std::move(entries)) | yframe;
    if (!focused) {
      pane |= color(Color::GrayDark);
    }
    return pane | size(WIDTH, EQUAL, 20);
  });

  auto focus_tab1_left = [&] {
    tab1_left->TakeFocus();
  };
  auto focus_tab1_top = [&] {
    tab1_top->TakeFocus();
  };

  auto tab1_layout = Container::Horizontal({tab1_left, tab1_right});
  auto tab1 = Renderer(tab1_layout, [&] {
    return hbox({
               tab1_left->Render() | size(WIDTH, EQUAL, 20),
               separator(),
               tab1_right->Render() | flex,
           }) |
           flex;
  });
  tab1 = CatchEvent(tab1, [&](Event event) {
    if (event == Event::Tab) {
      if (tab1_left->Focused()) {
        focus_tab1_top();
      } else if (tab1_top->Focused()) {
        focus_tab1_input();
      } else {
        focus_tab1_left();
      }
      return true;
    }
    if (event == Event::TabReverse) {
      if (tab1_left->Focused()) {
        focus_tab1_input();
      } else if (tab1_top->Focused()) {
        focus_tab1_left();
      } else {
        focus_tab1_top();
      }
      return true;
    }
    if (!IsSubmitShortcut(event)) {
      return false;
    }
    append_tab1_input();
    return true;
  });

  std::vector<std::string> tab2_entries = {
      "Alpha",
      "Bravo",
      "Charlie",
      "Delta",
      "Echo",
  };
  int tab2_selected = 0;
  auto tab2_list = Menu(&tab2_entries, &tab2_selected);
  auto tab2 = Renderer(tab2_list, [&] {
    return tab2_list->Render() | frame;
  });

  auto tab3 = RichLogView({
      .data = &tab3_log_data,
      .scroll = &tab3_scroll,
      .overscan = 1,
  });

  int selected_tab = 0;
  auto tab_container = Container::Tab({tab1, tab2, tab3}, &selected_tab);
  auto shortcuts = CatchEvent(tab_container, [&](Event event) {
    if (event == Event::F1) {
      selected_tab = 0;
      focus_tab1_input();
      return true;
    }
    if (event == Event::F2) {
      selected_tab = 1;
      return true;
    }
    if (event == Event::F3) {
      selected_tab = 2;
      return true;
    }
    return false;
  });

  auto component = Renderer(shortcuts, [&] {
    return tab_container->Render() | flex;
  });

  std::atomic<bool> worker_running = true;
  std::thread tab1_left_worker([&] {
    uint32_t worker_seed = 0x9E3779B9u;
    while (worker_running) {
      auto lines = MakeRandomTab1LeftLines(&worker_seed);
      screen.Post([&app_state, lines = std::move(lines)]() mutable {
        app_state.tab1_left_lines = std::move(lines);
      });
      screen.PostEvent(Event::Custom);

      for (int i = 0; i < 10 && worker_running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });

  focus_tab1_input();
  screen.Loop(component);
  worker_running = false;
  tab1_left_worker.join();
}
