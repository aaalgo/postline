#pragma once

#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <postline/program.h>

namespace postline { namespace ui {

struct Tab {
    virtual ~Tab() = default;
    virtual std::string label() const = 0;
    virtual ftxui::Component component() = 0;
    virtual bool onShortcut(ftxui::Event const &event);
    virtual ThreadID threadId() const;
};

}}
