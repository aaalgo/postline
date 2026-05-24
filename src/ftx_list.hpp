#ifndef FTX_LIST_HPP
#define FTX_LIST_HPP

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/util/ref.hpp>

#include <postline/common.h>

namespace ftxui {
    using postline::check_fail;

class ListDataRef {
 public:
  virtual ~ListDataRef() = default;

  virtual int firstValid() const = 0;
  virtual int end() const = 0;
  virtual Element Render(int index) const = 0;
};

template <typename T>
class ListData : public ListDataRef {
  using Renderer = std::function<Element(T const&)>;

  std::deque<T> items_;
  size_t limit_;
  int first_valid_ = 0;
  Renderer renderer_;

  void TrimToLimit() {
    while (items_.size() > limit_) {
      items_.pop_front();
      first_valid_++;
    }
  }

 public:
  explicit ListData(size_t limit, Renderer renderer)
      : limit_(limit),
        renderer_(std::move(renderer)) {
    CHECK(limit_ > 0);
    CHECK(renderer_);
  }

  int firstValid() const override { return first_valid_; }

  int end() const override { return first_valid_ + int(items_.size()); }

  size_t size () const {
      return size_t(end());
  }

  bool contains(int index) const {
    return index >= firstValid() && index < end();
  }

  T const& at(int index) const {
    CHECK(contains(index));
    return items_[index - first_valid_];
  }

  Element Render(int index) const override {
    return renderer_(at(index));
  }

  void push_back(T const &item) {
    items_.push_back(item);
    TrimToLimit();
  }

  void push_back(T&& item) {
    items_.push_back(std::move(item));
    TrimToLimit();
  }

  size_t retainedSize() const { return items_.size(); }

  size_t limit() const { return limit_; }
};

struct ListOption {
  ListDataRef* entries = nullptr;

  // A negative value means there is no selected item.
  Ref<int> selected = -1;

  // When enabled, the viewport follows the end of [firstValid(), end()).
  Ref<bool> follow_tail = true;

  std::function<Element()> empty_element;

  std::function<void()> on_change;
  std::function<void()> on_enter;
  std::function<void()> on_open_selection;
  std::function<void(int old_selected, int new_selected)> on_selected_change;
};

namespace detail {

class ListBase : public ComponentBase, public ListOption {
  int first_visible_ = 0;

  std::vector<Box> boxes_;
  std::vector<int> box_indexes_;
  Box viewport_box_;

  Element OnRender() override {
    NormalizeSelection(/*notify=*/true);
    UpdateViewport();

    Elements elements;
    boxes_.clear();
    box_indexes_.clear();

    if (empty()) {
      Element element = empty_element ? empty_element() : text("(empty)");
      return element | reflect(viewport_box_);
    }

    const int last_visible = std::min(end(), first_visible_ + VisibleHeight());
    const int visible_count = last_visible - first_visible_;
    boxes_.reserve(visible_count);
    box_indexes_.reserve(visible_count);

    for (int i = first_visible_; i < last_visible; ++i) {
      boxes_.emplace_back();
      box_indexes_.push_back(i);

      const bool active = selected() == i;
      Element element = entries->Render(i);
      if (active && Focused()) {
        element |= inverted;
      }
      if (active) {
        element |= bold;
        element |= focus;
      }
      elements.push_back(element | reflect(boxes_.back()));
    }

    return vbox(std::move(elements)) | yframe | reflect(viewport_box_);
  }

  bool OnEvent(Event event) override {
    if (!CaptureMouse(event)) {
      return false;
    }

    if (event.is_mouse()) {
      return OnMouseEvent(event);
    }

    if (!Focused() || empty()) {
      return false;
    }

    const int old_selected = selected();
    const bool old_follow_tail = follow_tail();

    if (event == Event::ArrowUp || event == Event::Character('k')) {
      if (selected() < 0) {
        selected() = end() - 1;
      } else {
        selected() = std::max(firstValid(), selected() - 1);
      }
      follow_tail() = false;
    }

    if (event == Event::ArrowDown || event == Event::Character('j')) {
      if (follow_tail()) {
        return false;
      }

      if (selected() < 0) {
        selected() = firstValid();
        follow_tail() = false;
      } else if (selected() + 1 >= end()) {
        selected() = -1;
        follow_tail() = true;
      } else {
        selected()++;
        follow_tail() = false;
      }
    }

    if (event == Event::PageUp) {
      SelectFromTailOrViewport(/*from_bottom=*/true);
      selected() = std::max(firstValid(), selected() - PageStep());
      follow_tail() = false;
    }

    if (event == Event::PageDown) {
      if (follow_tail()) {
        return false;
      }

      if (selected() < 0) {
        SelectFromViewport(/*from_bottom=*/false);
        follow_tail() = false;
      } else if (selected() + PageStep() >= end()) {
        selected() = -1;
        follow_tail() = true;
      } else {
        selected() += PageStep();
        follow_tail() = false;
      }
    }

    if (event == Event::Home) {
      selected() = firstValid();
      follow_tail() = false;
    }

    if (event == Event::End) {
      selected() = -1;
      follow_tail() = true;
    }

    if (event == Event::Tab) {
      if (selected() < 0) {
        SelectFromViewport(/*from_bottom=*/false);
        follow_tail() = false;
      } else if (selected() + 1 < end()) {
        selected()++;
        follow_tail() = false;
      }
    }

    if (event == Event::TabReverse) {
      SelectFromTailOrViewport(/*from_bottom=*/true);
      selected() = std::max(firstValid(), selected() - 1);
      follow_tail() = false;
    }

    NormalizeSelection(/*notify=*/false);

    if (selected() != old_selected) {
      OnSelectedChange(old_selected, selected());
    }

    if (selected() != old_selected || follow_tail() != old_follow_tail) {
      OnChange();
      return true;
    }

    if (event == Event::Return && selected() >= 0) {
      OnEnter();
      OnOpenSelection();
      return true;
    }

    return false;
  }

  bool Focusable() const final {
    return entries != nullptr && entries->firstValid() < entries->end();
  }

 private:
  int firstValid() const { return entries->firstValid(); }
  int end() const { return entries->end(); }
  bool empty() const { return entries == nullptr || firstValid() >= end(); }

  int VisibleHeight() const {
    if (viewport_box_.y_max >= viewport_box_.y_min) {
      return std::max(1, viewport_box_.y_max - viewport_box_.y_min + 1);
    }
    return 10;
  }

  int PageStep() const { return std::max(1, VisibleHeight() - 1); }

  int LastFirstVisible() const {
    return std::max(firstValid(), end() - VisibleHeight());
  }

  void NormalizeSelection(bool notify) {
    if (entries == nullptr || firstValid() >= end()) {
      SetSelectedIfChanged(-1, notify);
      return;
    }

    if (selected() < firstValid() || selected() >= end()) {
      SetSelectedIfChanged(-1, notify);
    }

    if (selected() >= 0) {
      follow_tail() = false;
    }
  }

  void UpdateViewport() {
    if (empty()) {
      first_visible_ = entries == nullptr ? 0 : firstValid();
      return;
    }

    if (follow_tail()) {
      first_visible_ = LastFirstVisible();
      return;
    }

    if (selected() >= 0) {
      if (selected() < first_visible_) {
        first_visible_ = selected();
      }

      if (selected() >= first_visible_ + VisibleHeight()) {
        first_visible_ = selected() - VisibleHeight() + 1;
      }
    }

    first_visible_ =
        std::clamp(first_visible_, firstValid(), LastFirstVisible());
  }

  void SelectFromViewport(bool from_bottom) {
    const int last_visible = std::min(end(), first_visible_ + VisibleHeight());
    if (from_bottom) {
      selected() = std::max(firstValid(), last_visible - 1);
      return;
    }
    selected() = first_visible_;
  }

  void SelectFromTailOrViewport(bool from_bottom) {
    if (follow_tail()) {
      selected() = end() - 1;
      return;
    }

    if (selected() < 0) {
      SelectFromViewport(from_bottom);
    }
  }

  bool OnMouseEvent(Event event) {
    if (event.mouse().button == Mouse::WheelDown ||
        event.mouse().button == Mouse::WheelUp) {
      return OnMouseWheel(event);
    }

    if (event.mouse().button != Mouse::None &&
        event.mouse().button != Mouse::Left) {
      return false;
    }

    for (size_t i = 0; i < boxes_.size(); ++i) {
      if (!boxes_[i].Contain(event.mouse().x, event.mouse().y)) {
        continue;
      }

      TakeFocus();

      if (event.mouse().button == Mouse::Left &&
          event.mouse().motion == Mouse::Pressed) {
        const int old_selected = selected();
        selected() = box_indexes_[i];
        follow_tail() = false;
        if (selected() != old_selected) {
          OnSelectedChange(old_selected, selected());
          OnChange();
        }
        return true;
      }

      return false;
    }

    return false;
  }

  bool OnMouseWheel(Event event) {
    if (!viewport_box_.Contain(event.mouse().x, event.mouse().y) || empty()) {
      return false;
    }

    const int old_selected = selected();
    const bool old_follow_tail = follow_tail();

    if (event.mouse().button == Mouse::WheelUp) {
      SelectFromTailOrViewport(/*from_bottom=*/true);
      selected() = std::max(firstValid(), selected() - 1);
      follow_tail() = false;
    }

    if (event.mouse().button == Mouse::WheelDown) {
      if (selected() < 0) {
        SelectFromViewport(/*from_bottom=*/false);
        follow_tail() = false;
      } else if (selected() + 1 >= end()) {
        selected() = -1;
        follow_tail() = true;
      } else {
        selected()++;
        follow_tail() = false;
      }
    }

    NormalizeSelection(/*notify=*/false);

    if (selected() != old_selected) {
      OnSelectedChange(old_selected, selected());
    }

    if (selected() != old_selected || follow_tail() != old_follow_tail) {
      OnChange();
    }

    return true;
  }

  void SetSelectedIfChanged(int value, bool notify) {
    const int old_selected = selected();
    selected() = value;
    if (notify && old_selected != selected()) {
      OnSelectedChange(old_selected, selected());
      OnChange();
    }
  }

  void OnChange() {
    if (on_change) {
      on_change();
    }
  }

  void OnEnter() {
    if (on_enter) {
      on_enter();
    }
  }

  void OnOpenSelection() {
    if (on_open_selection) {
      on_open_selection();
    }
  }

  void OnSelectedChange(int old_selected, int new_selected) {
    if (on_selected_change) {
      on_selected_change(old_selected, new_selected);
    }
  }

 public:
  explicit ListBase(ListOption option) : ListOption(std::move(option)) {}
};

}  // namespace detail

inline Component List(ListOption option) {
  return Make<detail::ListBase>(std::move(option));
}

inline Component List(ListDataRef* entries,
                      int* selected,
                      bool* follow_tail,
                      ListOption option = {}) {
  option.entries = entries;
  option.selected = selected;
  option.follow_tail = follow_tail;
  return List(std::move(option));
}

}  // namespace ftxui

#endif
