#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <postline/common.h>

namespace postline { namespace ui {

class MessageReader {
    Message const **message;

    int message_scroll = 0;
    int message_line_count = 0;
    ftxui::Box message_viewport_box;

    ftxui::Component renderer;

    int messageViewportHeight() const;
    int maxMessageScroll() const;
    void scrollMessageBy(int delta);
    ftxui::Element renderMessage();
    bool onMessageViewerEvent(ftxui::Event event);
    bool onMessageViewerMouseEvent(ftxui::Event event);

public:
    explicit MessageReader(Message const **message_);

    void resetScroll();
    ftxui::Component component();
};

}}
