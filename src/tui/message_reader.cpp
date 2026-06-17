#include "message_reader.h"

#include <algorithm>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include "render.h"

namespace postline { namespace ui {

using namespace ftxui;

int MessageReader::messageViewportHeight() const {
    if (message_viewport_box.y_max >= message_viewport_box.y_min) {
        return std::max(1, message_viewport_box.y_max -
                               message_viewport_box.y_min + 1);
    }
    return 10;
}

int MessageReader::maxMessageScroll() const {
    return std::max(0, message_line_count - messageViewportHeight());
}

void MessageReader::scrollMessageBy(int delta) {
    message_scroll = std::clamp(message_scroll + delta, 0,
                                maxMessageScroll());
}

Element MessageReader::renderMessage() {
    Elements lines;
    if (!(*message)->header().contains(CONTEXT_HEADER_NAME)) {
        return vbox(std::move(lines));
    }
    json const &header = (*message)->header();

    appendMessageHeaders(lines, header);
    lines.push_back(text(""));
    appendTextLines(lines, (*message)->body());
    message_line_count = int(lines.size());
    message_scroll = std::clamp(message_scroll, 0, maxMessageScroll());

    return vbox(std::move(lines));
}

bool MessageReader::onMessageViewerEvent(Event event) {
    if (event.is_mouse()) {
        return onMessageViewerMouseEvent(event);
    }

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        scrollMessageBy(-1);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        scrollMessageBy(1);
        return true;
    }
    if (event == Event::PageUp) {
        scrollMessageBy(-std::max(1, messageViewportHeight() - 1));
        return true;
    }
    if (event == Event::PageDown) {
        scrollMessageBy(std::max(1, messageViewportHeight() - 1));
        return true;
    }
    if (event == Event::Home) {
        message_scroll = 0;
        return true;
    }
    if (event == Event::End) {
        message_scroll = maxMessageScroll();
        return true;
    }
    return false;
}

bool MessageReader::onMessageViewerMouseEvent(Event event) {
    if (event.mouse().button != Mouse::WheelUp &&
        event.mouse().button != Mouse::WheelDown) {
        return false;
    }

    if (!message_viewport_box.Contain(event.mouse().x, event.mouse().y)) {
        return false;
    }

    if (event.mouse().button == Mouse::WheelUp) {
        scrollMessageBy(-1);
        return true;
    }

    scrollMessageBy(1);
    return true;
}

MessageReader::MessageReader(Message const **message_)
    : message(message_) {
    renderer = Renderer([&](bool) {
        return renderMessage() |
            focusPosition(0, message_scroll + messageViewportHeight() / 2) |
            vscroll_indicator |
            yframe |
            reflect(message_viewport_box);
    });
    renderer |= CatchEvent([&](Event event) {
        return onMessageViewerEvent(event);
    });
}

void MessageReader::resetScroll() {
    message_scroll = 0;
}

Component MessageReader::component() {
    return renderer;
}

}}
