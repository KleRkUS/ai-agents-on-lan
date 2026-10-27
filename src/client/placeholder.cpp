#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "ui.hpp"

namespace aio {

using namespace ftxui;

int run_placeholder() {
    auto screen = ScreenInteractive::Fullscreen();
    auto view = Renderer([] {
        return vbox({
                   text("no agent selected") | bold | center,
                   text(""),
                   text("press n in the sidebar to start one") | dim | center,
               }) |
               center | flex;
    });
    view |= CatchEvent([](Event) { return true; });
    screen.Loop(view);
    return 0;
}

}  // namespace aio
