#include "thread_tab.h"

#include <algorithm>
#include <format>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <postline/common.h>

#include "render.h"

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

Component borderlessSplitLeft(Component main, Component back, int *main_size) {
    return Make<BorderlessSplitLeft>(std::move(main),
                                     std::move(back),
                                     main_size);
}

}

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

    /*
    if (header.contains(CONTEXT_HEADER_NAME)) {
        appendTextLines(lines, header.at(CONTEXT_HEADER_NAME).dump(2));
    }
    */

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

MessageEditor::MessageEditor(Thread const *thread_,
                             AddressProvider address_provider_,
                             SendCallback on_send_)
    : thread(thread_),
      address_provider(std::move(address_provider_)),
      on_send(std::move(on_send_)) {
    syncAddresses();

    DropdownOption address_option;
    address_option.radiobox.entries = &address_labels;
    address_option.radiobox.selected = &address_selected;
    address_option.checkbox.transform = [](EntryState const &state) {
        Element element = hbox({
            text(state.state ? "v " : "> ") | color(Color::GrayLight),
            text(state.label),
        });
        if (state.focused) {
            element |= inverted;
        }
        return element;
    };
    address_option.radiobox.transform = [](EntryState const &state) {
        Element element = text((state.active ? "> " : "  ") + state.label);
        if (state.focused) {
            element |= inverted;
        }
        if (state.active) {
            element |= bold;
        }
        return element;
    };
    address_option.transform = [](bool is_open,
                                  Element checkbox_element,
                                  Element radiobox_element) {
        if (is_open) {
            return vbox({
                std::move(checkbox_element),
                std::move(radiobox_element) |
                    vscroll_indicator |
                    frame |
                    size(HEIGHT, LESS_THAN, 6),
            });
        }
        return std::move(checkbox_element);
    };
    address_choice = Dropdown(std::move(address_option));

    ButtonOption send_option;
    send_option.label = "Send";
    send_option.on_click = [this] {
        CHECK(!thread->pending);
        CHECK(!address_agents.empty());
        CHECK(address_agents.size() == address_labels.size());
        CHECK(address_selected >= 0);
        CHECK(address_selected < int(address_agents.size()));

        on_send(address_agents[address_selected],
                subject_content,
                body_content);
        subject_content.clear();
        body_content.clear();
    };
    send_option.transform = [this](EntryState const &state) {
        Element element = text(state.label);
        if (thread->pending || address_agents.empty()) {
            return element | color(Color::GrayDark) | dim;
        }

        element |= color(Color::GreenLight) | bold;
        if (state.focused) {
            element |= inverted;
        }
        return element;
    };
    send_button = Button(std::move(send_option));
    send_button |= CatchEvent([this](Event event) {
        if (!thread->pending && !address_agents.empty()) {
            return false;
        }
        if (event == Event::Return || event.is_mouse()) {
            return true;
        }
        return false;
    });

    auto subject_option = InputOption::Default();
    subject_option.multiline = false;
    subject_option.transform = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        return state.element;
    };
    subject_editor = Input(&subject_content, "subject", subject_option);

    auto body_option = InputOption::Default();
    body_option.multiline = true;
    body_option.transform = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        return state.element;
    };
    body_editor = Input(&body_content, body_option);

    renderer = Renderer(
        Container::Vertical({
            address_choice,
            send_button,
            subject_editor,
            body_editor,
        }),
        [&] {
            syncAddresses();

            return vbox({
                hbox({
                    text("To: ") | color(Color::Cyan) | bold,
                    address_choice->Render() | flex,
                    filler(),
                    send_button->Render(),
                }),
                hbox({
                    text("Subject: ") | color(Color::Magenta) | bold,
                    subject_editor->Render() | flex,
                }),
                body_editor->Render() | flex,
            }) | flex;
        });
}

void MessageEditor::syncAddresses() {
    CHECK(thread->root);
    Domain const *domain = thread->root;
    if (!thread->stack.empty()) {
        domain = thread->stack.back().to.domain;
    }
    CHECK(domain);

    std::string selected_name;
    if (address_selected >= 0 &&
        address_selected < int(address_agents.size())) {
        CHECK(address_agents[address_selected]);
        selected_name = address_agents[address_selected]->name;
    }

    address_agents.clear();
    address_agents.reserve(domain->members.size());
    for (auto const &[name, agent]: domain->members) {
        CHECK(agent);
        CHECK(name == agent->name);
        address_agents.push_back(agent);
    }
    address_provider(&address_agents);
    for (Agent const *agent: address_agents) {
        CHECK(agent);
    }

    std::stable_sort(
        address_agents.begin(),
        address_agents.end(),
        [](Agent const *lhs, Agent const *rhs) {
            return lhs->name < rhs->name;
        });
    auto last = std::unique(
        address_agents.begin(),
        address_agents.end(),
        [](Agent const *lhs, Agent const *rhs) {
            return lhs == rhs || lhs->name == rhs->name;
        });
    address_agents.erase(last, address_agents.end());

    address_labels.clear();
    address_labels.reserve(address_agents.size());
    for (Agent const *agent: address_agents) {
        CHECK(agent);
        address_labels.push_back(agent->name);
    }

    if (address_agents.empty()) {
        address_selected = 0;
        return;
    }

    auto selected = std::find_if(
        address_agents.begin(),
        address_agents.end(),
        [&selected_name](Agent const *agent) {
            CHECK(agent);
            return agent->name == selected_name;
        });
    if (selected != address_agents.end()) {
        address_selected = int(selected - address_agents.begin());
        return;
    }

    address_selected = std::clamp(
        address_selected, 0, int(address_agents.size()) - 1);
}

bool MessageEditor::sendCurrentMessage() {
    if (thread->pending) {
        return false;
    }
    if (address_agents.empty()) {
        return false;
    }
    if (subject_content.empty() && body_content.empty()) {
        return false;
    }

    CHECK(address_agents.size() == address_labels.size());
    CHECK(address_selected >= 0);
    CHECK(address_selected < int(address_agents.size()));

    on_send(address_agents[address_selected],
            subject_content,
            body_content);
    subject_content.clear();
    body_content.clear();
    return true;
}

Component MessageEditor::component() {
    return renderer;
}

void ThreadTab::appendTreeEntries(Domain const *domain, size_t depth) {
    CHECK(domain);
    std::string label = std::string(depth * 2, ' ') + domain->name;
    bool pruned = depth > 0 && domain->detached;
    tree_entries.push_back(label);
    tree_metadata.push_back({std::move(label), pruned});

    if (pruned) {
        return;
    }

    std::vector<Domain const *> children;
    children.reserve(domain->children.size());
    for (auto const &[name, child]: domain->children) {
        CHECK(child);
        CHECK(name == child->name);
        children.push_back(child);
    }
    std::sort(
        children.begin(),
        children.end(),
        [](Domain const *lhs, Domain const *rhs) {
            return lhs->name < rhs->name;
        });

    for (Domain const *child: children) {
        appendTreeEntries(child, depth + 1);
    }
}

void ThreadTab::syncTreeEntries() {
    CHECK(data->root);
    tree_entries.clear();
    tree_metadata.clear();
    appendTreeEntries(data->root, 0);

    if (nav_mode != 0) {
        return;
    }
    nav_selected = std::clamp(
        nav_selected, 0, int(tree_entries.size()) - 1);
}

void ThreadTab::syncStackEntries() {
    stack_entries.clear();
    stack_entries.reserve(data->stack.size());

    for (Frame const &frame: data->stack) {
        CHECK(frame.from.agent);
        CHECK(frame.from.domain);
        CHECK(frame.to.agent);
        CHECK(frame.to.domain);

        stack_entries.push_back(std::format(
            "{}@{} -> {}@{}",
            frame.from.agent->name,
            frame.from.domain->name,
            frame.to.agent->name,
            frame.to.domain->name));
    }

    if (nav_mode != 1) {
        return;
    }
    if (stack_entries.empty()) {
        nav_selected = 0;
        return;
    }
    nav_selected = std::clamp(
        nav_selected, 0, int(stack_entries.size()) - 1);
}

void ThreadTab::syncMemberEntries() {
    CHECK(data->root);
    Domain const *domain = data->root;
    if (!data->stack.empty()) {
        domain = data->stack.back().to.domain;
        CHECK(domain);
    }

    member_entries.clear();
    member_entries.reserve(
        domain->members.size() + domain->children.size());

    for (auto const &[name, agent]: domain->members) {
        CHECK(agent);
        CHECK(name == agent->name);
        member_entries.push_back(agent->name);
    }
    std::sort(member_entries.begin(), member_entries.end());

    std::vector<std::string> child_entries;
    child_entries.reserve(domain->children.size());
    for (auto const &[name, child]: domain->children) {
        CHECK(child);
        CHECK(name == child->name);
        child_entries.push_back("@" + child->name);
    }
    std::sort(child_entries.begin(), child_entries.end());
    member_entries.insert(
        member_entries.end(),
        child_entries.begin(),
        child_entries.end());

    if (nav_mode != 2) {
        return;
    }
    if (member_entries.empty()) {
        nav_selected = 0;
        return;
    }
    nav_selected = std::clamp(
        nav_selected, 0, int(member_entries.size()) - 1);
}

void ThreadTab::reloadCurrentMessage() {
    AccessID next_id = NO_ACCESS_ID;

    if (trace_selected >= 0 &&
        trace_selected < int(data->trace.size())) {
        next_id = data->trace.at(trace_selected);
    }

    if (next_id == current_message_id) {
        return;
    }

    current_message = read_message(next_id);
    current_message_id = next_id;
    message_reader.resetScroll();
}

void ThreadTab::focusPane() {
    switch (pane_focus.focusedPane()) {
    case ThreadPaneFocus::FocusedPane::Navigation:
        left_renderer->TakeFocus();
        return;
    case ThreadPaneFocus::FocusedPane::Trace:
        trace_list->TakeFocus();
        return;
    case ThreadPaneFocus::FocusedPane::Reader:
        message_reader.component()->TakeFocus();
        return;
    case ThreadPaneFocus::FocusedPane::Composer:
        message_editor.component()->TakeFocus();
        return;
    }
    CHECK(0);
}

bool ThreadTab::onFocusEvent(Event const &event) {
    if (left_renderer->Focused()) {
        pane_focus.setFocusedPane(
            ThreadPaneFocus::FocusedPane::Navigation);
    }
    else if (trace_list->Focused()) {
        pane_focus.setFocusedPane(ThreadPaneFocus::FocusedPane::Trace);
    }
    else if (message_reader.component()->Focused()) {
        pane_focus.setFocusedPane(ThreadPaneFocus::FocusedPane::Reader);
    }
    else if (message_editor.component()->Focused()) {
        pane_focus.setFocusedPane(ThreadPaneFocus::FocusedPane::Composer);
    }

    if (!pane_focus.onEvent(event)) {
        return false;
    }
    focusPane();
    return true;
}

bool ThreadTab::paneFocused(ThreadPaneFocus::FocusedPane pane) {
    switch (pane) {
    case ThreadPaneFocus::FocusedPane::Navigation:
        return left_renderer->Focused();
    case ThreadPaneFocus::FocusedPane::Trace:
        return trace_list->Focused();
    case ThreadPaneFocus::FocusedPane::Reader:
        return message_reader.component()->Focused();
    case ThreadPaneFocus::FocusedPane::Composer:
        return message_editor.component()->Focused();
    }
    CHECK(0);
}

Element ThreadTab::renderPane(std::string title,
                              Element content,
                              bool focused) const {
    return renderWindowPane(std::move(title), std::move(content), focused);
}

void ThreadTab::toggleMaximizeRightPane() {
    if (right_pane_mode != RightPaneMode::Normal) {
        right_pane_mode = RightPaneMode::Normal;
        return;
    }

    if (message_editor.component()->Focused()) {
        right_pane_mode = RightPaneMode::Editor;
        message_editor.component()->TakeFocus();
        return;
    }

    right_pane_mode = RightPaneMode::Message;
    message_reader.component()->TakeFocus();
}

ThreadTab::ThreadTab(Observer *observer, Thread *data_,
                     std::function<Message const *(AccessID)> read_message_,
                     std::function<void(ThreadID, Message&&)> on_send_)
    : data(data_),
      user(observer->user),
      trace(observer, &data_->trace),
      read_message(std::move(read_message_)),
      on_send(std::move(on_send_)),
      message_reader(&current_message),
      message_editor(
          data_,
          [observer](std::vector<Agent *> *agents) {
              CHECK(agents);
              CHECK(observer->runtime);
              agents->push_back(observer->runtime);
          },
          [this](Agent *to, std::string subject, std::string body) {
              CHECK(to);
              json header{{"To", to->name},
                          {"Subject", std::move(subject)}};
              if (!data->stack.empty()) {
                  Frame const &frame = data->stack.back();
                  CHECK(frame.to.agent);
                  CHECK(frame.from.agent);
                  if (frame.to.agent == user &&
                      frame.from.agent == to) {
                      header["In-Reply-To"] =
                          std::format("{}", frame.message_id);
                  }
              }
              on_send(data->id,
                      Message(std::move(header), std::move(body)));
          }) {
    CHECK(user);

    nav_mode_menu = Menu(
        &nav_modes,
        &nav_mode,
        MenuOption::Horizontal());

    MenuOption tree_option;
    tree_option.entries_option.transform = [this](EntryState const &state) {
        CHECK(state.index >= 0);
        CHECK(state.index < int(tree_metadata.size()));
        TreeEntry const &entry = tree_metadata.at(state.index);
        CHECK(state.label == entry.label);

        Element element = text((state.active ? "> " : "  ") + state.label) |
                          color(entry.pruned ? theme::muted() : theme::text());
        if (entry.pruned) {
            element |= dim;
        }
        if (state.focused) {
            element |= inverted;
        }
        if (state.active) {
            element |= bold;
        }
        return element;
    };
    tree_list = Menu(&tree_entries, &nav_selected, tree_option);
    stack_list = Menu(&stack_entries, &nav_selected);
    member_list = Menu(&member_entries, &nav_selected);
    nav_content = Container::Tab({
        tree_list,
        stack_list,
        member_list,
    }, &nav_mode);

    trace_list = List(&trace,
                       &trace_selected,
                       &trace_follow_tail);

    left_renderer = Renderer(
        Container::Vertical({
            nav_mode_menu,
            nav_content,
        }),
        [&] {
            syncTreeEntries();
            syncStackEntries();
            syncMemberEntries();

            Element thread_name = text("");
            if (!data->name.empty()) {
                thread_name = hbox({
                    text(" "),
                    text(data->name),
                });
            }

            return renderPane(
                "Navigation",
                vbox({
                    hbox({
                        text("thread ") | bold,
                        text(std::format("{}", data->id)) |
                            color(Color::Yellow),
                        thread_name,
                    }),
                    nav_mode_menu->Render(),
                    separator(),
                    nav_content->Render() | flex,
                }),
                paneFocused(ThreadPaneFocus::FocusedPane::Navigation)) |
                reflect(navigation_pane_box);
        });

    middle_renderer = Renderer(
        trace_list,
        [&] {
            return renderPane(
                "Trace",
                trace_list->Render(),
                paneFocused(ThreadPaneFocus::FocusedPane::Trace)) |
                reflect(trace_pane_box);
        });

    right_renderer = Renderer(
        Container::Vertical({
            message_reader.component(),
            message_editor.component(),
        }),
        [&] {
            reloadCurrentMessage();
            return vbox({
                renderPane(
                    "Reader",
                    message_reader.component()->Render() | flex,
                    paneFocused(ThreadPaneFocus::FocusedPane::Reader)) |
                    reflect(reader_pane_box) | flex,
                renderPane(
                    "Composer",
                    message_editor.component()->Render(),
                    paneFocused(ThreadPaneFocus::FocusedPane::Composer)) |
                    reflect(composer_pane_box) |
                    size(HEIGHT, EQUAL, 12),
            });
        });

    auto middle_right_split =
        borderlessSplitLeft(middle_renderer, right_renderer,
                            &middle_column_width);
    root = borderlessSplitLeft(left_renderer, middle_right_split,
                               &left_column_width);

    renderer = Renderer(root, [&] {
        if (right_pane_mode == RightPaneMode::Message) {
            reloadCurrentMessage();
            return renderPane(
                "Reader",
                message_reader.component()->Render() | flex,
                paneFocused(ThreadPaneFocus::FocusedPane::Reader)) |
                reflect(reader_pane_box) | flex;
        }
        if (right_pane_mode == RightPaneMode::Editor) {
            return renderPane(
                "Composer",
                message_editor.component()->Render() | flex,
                paneFocused(ThreadPaneFocus::FocusedPane::Composer)) |
                reflect(composer_pane_box) | flex;
        }

        return root->Render() | flex;
    });
    renderer |= CatchEvent([this](Event event) {
        if (event.is_mouse() &&
            event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Pressed) {
            if (navigation_pane_box.Contain(event.mouse().x, event.mouse().y)) {
                left_renderer->TakeFocus();
            }
            else if (trace_pane_box.Contain(event.mouse().x, event.mouse().y)) {
                trace_list->TakeFocus();
            }
            else if (reader_pane_box.Contain(event.mouse().x, event.mouse().y)) {
                message_reader.component()->TakeFocus();
            }
            else if (composer_pane_box.Contain(event.mouse().x,
                                              event.mouse().y)) {
                message_editor.component()->TakeFocus();
            }
        }
        return onFocusEvent(event);
    });
    focusPane();
}

std::string ThreadTab::label() const {
    return "T" + std::to_string(data->id);
}

Component ThreadTab::component() {
    return renderer;
}

bool ThreadTab::onShortcut(Event const &event) {
    if (event == Event::F12) {
        return message_editor.sendCurrentMessage();
    }
    if (event == Event::F10) {
        toggleMaximizeRightPane();
        return true;
    }
    return false;
}

ThreadID ThreadTab::threadId() const {
    return data->id;
}

}}
