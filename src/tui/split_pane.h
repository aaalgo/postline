#pragma once

#include <ftxui/component/component.hpp>

namespace postline { namespace ui {

ftxui::Component borderlessSplitLeft(ftxui::Component main,
                                      ftxui::Component back,
                                      int *main_size);

}}
