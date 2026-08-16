#include "message_editor.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <set>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <postline/common.h>

#include "thread_nav.h"

namespace postline { namespace ui {

using namespace ftxui;

namespace {

std::vector<std::string> parseTags(std::string const &input) {
    std::set<std::string> tags;
    size_t begin = 0;
    while (begin <= input.size()) {
        size_t end = input.find(',', begin);
        if (end == std::string::npos) {
            end = input.size();
        }

        auto first = input.begin() + begin;
        auto last = input.begin() + end;
        while (first != last && std::isspace(static_cast<unsigned char>(*first))) {
            ++first;
        }
        while (first != last && std::isspace(static_cast<unsigned char>(*(last - 1)))) {
            --last;
        }
        if (first != last) {
            tags.emplace(first, last);
        }

        if (end == input.size()) {
            break;
        }
        begin = end + 1;
    }
    return {tags.begin(), tags.end()};
}

std::string formatTags(std::vector<std::string> const &tags) {
    std::string result;
    for (std::string const &tag: tags) {
        if (!result.empty()) {
            result += ", ";
        }
        result += tag;
    }
    return result;
}

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

        sendMessageTo(address_agents[address_selected]);
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

    CheckboxOption clone_option;
    clone_option.label = "Clone";
    clone_option.checked = &clone_checked;
    clone_option.transform = [](EntryState const &state) {
        Element element = hbox({
            text(state.state ? "[X] " : "[ ] ") | bold,
            text(state.label),
        });
        if (state.focused) {
            element |= inverted;
        }
        return element;
    };
    clone_checkbox = Checkbox(std::move(clone_option));

    auto tags_option = InputOption::Default();
    tags_option.multiline = false;
    tags_option.transform = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        return state.element;
    };
    tags_editor = Input(&tags_content, "comma-separated tags", tags_option);

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
            clone_checkbox,
            subject_editor,
            tags_editor,
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
                    filler(),
                    clone_checkbox->Render(),
                }),
                hbox({
                    text("Tags: ") | color(Color::Yellow) | bold,
                    tags_editor->Render() | flex,
                }),
                body_editor->Render() | flex,
            }) | flex;
        });
}

void MessageEditor::syncAddresses() {
    std::string selected_name;
    if (address_selected >= 0 &&
        address_selected < int(address_agents.size())) {
        CHECK(address_agents[address_selected]);
        selected_name = address_agents[address_selected]->name;
    }

    address_agents = buildThreadAddressAgents(thread);
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

void MessageEditor::sendMessageTo(Agent *to) {
    CHECK(thread);
    CHECK(to);

    std::string to_address = to->name;
    if (clone_checked) {
        to_address.insert(to_address.begin(), ADDRESS_CHAR_CLONE);
    }

    std::vector<std::string> tags = parseTags(tags_content);
    json header{{"To", std::move(to_address)},
                {"Subject", std::move(subject_content)},
                {"Thread-ID", std::format("{}", thread->id)},
                {POSTLINE_TAGS_HEADER_NAME, tags}};
    on_send(Message(std::move(header), std::move(body_content)));
    clone_checked = false;
    tags_content = formatTags(tags);
    subject_content.clear();
    body_content.clear();
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

    sendMessageTo(address_agents[address_selected]);
    return true;
}

Component MessageEditor::component() {
    return renderer;
}

}}
