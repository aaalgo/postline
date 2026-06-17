#include "thread_focus.h"

#include <cassert>

#include <ftxui/component/event.hpp>

using postline::ui::ThreadPaneFocus;

int main() {
    using Pane = ThreadPaneFocus::FocusedPane;

    ThreadPaneFocus focus;
    assert(focus.focusedPane() == Pane::Navigation);

    assert(focus.onEvent(ftxui::Event::Tab));
    assert(focus.focusedPane() == Pane::Trace);
    assert(focus.onEvent(ftxui::Event::Tab));
    assert(focus.focusedPane() == Pane::Reader);
    assert(focus.onEvent(ftxui::Event::Tab));
    assert(focus.focusedPane() == Pane::Composer);
    assert(focus.onEvent(ftxui::Event::Tab));
    assert(focus.focusedPane() == Pane::Navigation);

    assert(focus.onEvent(ftxui::Event::TabReverse));
    assert(focus.focusedPane() == Pane::Composer);
    assert(focus.onEvent(ftxui::Event::TabReverse));
    assert(focus.focusedPane() == Pane::Reader);
    assert(focus.onEvent(ftxui::Event::TabReverse));
    assert(focus.focusedPane() == Pane::Trace);
    assert(focus.onEvent(ftxui::Event::TabReverse));
    assert(focus.focusedPane() == Pane::Navigation);

    assert(!focus.onEvent(ftxui::Event::F10));
    assert(!focus.onEvent(ftxui::Event::F12));
    assert(focus.focusedPane() == Pane::Navigation);

    return 0;
}
