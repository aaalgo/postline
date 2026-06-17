#include "split_pane.h"

#include <algorithm>
#include <utility>

#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

namespace postline { namespace ui {

using namespace ftxui;

namespace {

class BorderlessSplitLeft : public ComponentBase {
    Component main;
    Component back;
    int *main_size;
    CapturedMouse captured_mouse;
    Box box;

    bool onMouseEvent(Event event) {
        if (captured_mouse && event.mouse().motion == Mouse::Released) {
            captured_mouse.reset();
            return true;
        }

        if (event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Pressed &&
            onSplitBoundary(event.mouse().x) &&
            !captured_mouse) {
            captured_mouse = CaptureMouse(event);
            return true;
        }

        if (!captured_mouse) {
            return ComponentBase::OnEvent(event);
        }

        *main_size = std::max(0, event.mouse().x - box.x_min);
        return true;
    }

    bool onSplitBoundary(int x) const {
        int boundary = box.x_min + *main_size;
        return x == boundary || x == boundary - 1;
    }

public:
    BorderlessSplitLeft(Component main_, Component back_, int *main_size_)
        : main(std::move(main_)),
          back(std::move(back_)),
          main_size(main_size_) {
        Add(Container::Horizontal({
            main,
            back,
        }));
    }

    bool OnEvent(Event event) override {
        if (event.is_mouse()) {
            return onMouseEvent(std::move(event));
        }
        return ComponentBase::OnEvent(std::move(event));
    }

    Element OnRender() override {
        return hbox({
            main->Render() | size(WIDTH, EQUAL, *main_size),
            back->Render() | xflex,
        }) | reflect(box);
    }
};

}

Component borderlessSplitLeft(Component main, Component back, int *main_size) {
    return Make<BorderlessSplitLeft>(std::move(main),
                                     std::move(back),
                                     main_size);
}

}}
