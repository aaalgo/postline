#include <postline/ui.h>

namespace postline { namespace ui {
    std::unique_ptr<UI> make_web (Runtime *) {
        CHECK(0);
        return nullptr;
    }

}};

