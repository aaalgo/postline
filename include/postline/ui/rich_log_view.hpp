// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef EXAMPLES_COMPONENT_RICH_LOG_VIEW_HPP
#define EXAMPLES_COMPONENT_RICH_LOG_VIEW_HPP

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include "ftxui/screen/color.hpp"
#include "ftxui/util/ref.hpp"

namespace ftxui {

struct RichLogSlice {
  int total_lines = 0;
  int page_size = 0;
  int max_scroll = 0;
  int first_visible = 0;
  int first_rendered = 0;
  int last_rendered = 0;
};

class RichLogData {
 public:
  using NotifyCallback = std::function<void()>;

  virtual ~RichLogData() = default;

  virtual int size() const = 0;                         // locked/synchronized
  virtual std::string operator[](int index) const = 0;  // locked/synchronized
  virtual bool HasColor(int index) const { return false; }
  virtual Color ColorAt(int index) const { return Color::Default; }

  void SetOnChange(NotifyCallback callback) {
    std::lock_guard<std::mutex> lock(notify_mutex_);
    on_change_ = std::move(callback);
  }

  RichLogSlice Locate(int scroll, int page_size, int overscan) const {
    const int total_lines = std::max(0, size());
    const int clamped_page_size = std::max(1, page_size);
    const int max_scroll = std::max(0, total_lines - clamped_page_size);
    const int first_visible = std::clamp(scroll, 0, max_scroll);
    const int first_rendered = std::max(0, first_visible - overscan);
    const int last_rendered =
        std::min(total_lines, first_visible + clamped_page_size + overscan);

    return {
        total_lines,
        clamped_page_size,
        max_scroll,
        first_visible,
        first_rendered,
        last_rendered,
    };
  }

 protected:
  void NotifyChanged() {
    NotifyCallback callback;
    {
      std::lock_guard<std::mutex> lock(notify_mutex_);
      callback = on_change_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  mutable std::mutex notify_mutex_;
  NotifyCallback on_change_;
};

class StringLogData : public RichLogData {
 public:
  struct LineRecord {
    std::string text;
    bool has_color = false;
    Color color = Color::Default;
  };

  explicit StringLogData(int max_history_lines)
      : max_history_lines_(std::max(1, max_history_lines)),
        lines_(static_cast<size_t>(max_history_lines_)) {}

  void append(std::string_view line) {
    append(line, false, Color::Default);
  }

  void append(std::string_view line, Color color) {
    append(line, true, color);
  }

  int size() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

  std::string operator[](int index) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    const LineRecord* record = RecordAtLocked(index);
    return record ? record->text : "";
  }

  bool HasColor(int index) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    const LineRecord* record = RecordAtLocked(index);
    return record ? record->has_color : false;
  }

  Color ColorAt(int index) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    const LineRecord* record = RecordAtLocked(index);
    return record ? record->color : Color::Default;
  }

 private:
  void append(std::string_view line, bool has_color, Color color) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const int write_index =
          size_ < max_history_lines_ ? (start_ + size_) % max_history_lines_
                                     : start_;
      lines_[static_cast<size_t>(write_index)] =
          LineRecord{std::string(line), has_color, color};
      if (size_ < max_history_lines_) {
        ++size_;
      } else {
        start_ = (start_ + 1) % max_history_lines_;
      }
    }
    NotifyChanged();
  }

  const LineRecord* RecordAtLocked(int index) const {
    if (index < 0 || index >= size_) {
      return nullptr;
    }
    return &lines_[static_cast<size_t>((start_ + index) % max_history_lines_)];
  }

  mutable std::mutex mutex_;
  int max_history_lines_ = 1;
  std::vector<LineRecord> lines_;
  int start_ = 0;
  int size_ = 0;
};

struct RichLogViewOption {
  RichLogData* data = nullptr;
  Ref<int> scroll = 0;
  int overscan = 1;
};

inline Component RichLogView(RichLogViewOption option) {
  class RichLogViewBase : public ComponentBase, public RichLogViewOption {
   public:
    explicit RichLogViewBase(RichLogViewOption option)
        : RichLogViewOption(std::move(option)) {}

   private:
    Element OnRender() override {
      const int page_size = std::max(1, box_.y_max - box_.y_min + 1);
      const RichLogSlice slice =
          data ? data->Locate(scroll(), page_size, overscan)
               : RichLogSlice{.page_size = page_size};
      scroll() = slice.first_visible;

      Elements rows;
      if (slice.first_rendered > 0) {
        rows.push_back(emptyElement() | size(HEIGHT, EQUAL, slice.first_rendered));
      }
      for (int i = slice.first_rendered; i < slice.last_rendered; ++i) {
        Element row = text((*data)[i]);
        if (data->HasColor(i)) {
          row |= color(data->ColorAt(i));
        }
        rows.push_back(std::move(row));
      }
      if (slice.last_rendered < slice.total_lines) {
        rows.push_back(
            emptyElement() |
            size(HEIGHT, EQUAL, slice.total_lines - slice.last_rendered));
      }
      if (rows.empty()) {
        rows.push_back(text(""));
      }

      Element output = vbox(std::move(rows)) |
                       focusPosition(0, slice.first_visible) |
                       vscroll_indicator |
                       yframe |
                       reflect(box_);
      if (Focused()) {
        output |= focus;
      }
      return output;
    }

    bool OnEvent(Event event) override {
      if (event.is_mouse()) {
        return OnMouseEvent(event);
      }

      if (!Focused()) {
        return false;
      }

      if (event == Event::Home) {
        return ScrollHome();
      }
      if (event == Event::End) {
        return ScrollEnd();
      }
      if (event == Event::PageUp) {
        return ScrollBy(-PageSize());
      }
      if (event == Event::PageDown) {
        return ScrollBy(PageSize());
      }
      return false;
    }

    bool Focusable() const override { return true; }

    bool OnMouseEvent(Event event) {
      if (!box_.Contain(event.mouse().x, event.mouse().y)) {
        return false;
      }

      if (event.mouse().button == Mouse::Left &&
          event.mouse().motion == Mouse::Pressed) {
        TakeFocus();
        return true;
      }

      if (event.mouse().button == Mouse::WheelUp) {
        TakeFocus();
        return ScrollBy(-3);
      }
      if (event.mouse().button == Mouse::WheelDown) {
        TakeFocus();
        return ScrollBy(3);
      }
      return false;
    }

    int PageSize() const {
      return std::max(1, box_.y_max - box_.y_min + 1);
    }

    int MaxScroll() const {
      return data ? data->Locate(scroll(), PageSize(), overscan).max_scroll : 0;
    }

    bool ScrollBy(int delta) {
      const int previous = scroll();
      scroll() = std::clamp(scroll() + delta, 0, MaxScroll());
      return scroll() != previous;
    }

    bool ScrollHome() {
      const int previous = scroll();
      scroll() = 0;
      return scroll() != previous;
    }

    bool ScrollEnd() {
      const int previous = scroll();
      scroll() = MaxScroll();
      return scroll() != previous;
    }

    Box box_;
  };

  return Make<RichLogViewBase>(std::move(option));
}

}  // namespace ftxui

#endif  // EXAMPLES_COMPONENT_RICH_LOG_VIEW_HPP
