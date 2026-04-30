#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <postline/common.h>

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

namespace ftxui {
using postline::check_fail;

// CLI displays a compact email composer for the current conversation.
//
// Root layout: top row with To/Subject, body editor below.
// Hitting Enter outside the body editor, or Alt+Enter/F12 anywhere,
// sends the current draft when canSend_ is true.
class CLI {
    std::string from_;
    std::unordered_set<std::string> to_list_;
    std::vector<std::string> to_entries_;
    std::string subject_;
    std::string body_;
    int to_selected_ = 0;
    bool can_send_ = false;

    std::function<void(postline::Message &&)> send_callback_;
    std::mutex mutex_;
    std::vector<postline::Message> pending_messages_;
    ScreenInteractive* screen_ = nullptr;
    Closure exit_loop_;

    Component to_dropdown_;
    Component subject_input_;
    Component body_input_;
    Component exit_button_;

    static bool IsSubmitShortcut(Event event) {
        return event == Event::F12 ||
               event == Event::CtrlAltM ||
               event.input() == "\x1B\r" ||
               event.input() == "\x1B\n" ||
               event.input() == "\x1B[27;3;13~" ||
               event.input() == "\x1B[13;3u";
    }

    static void trimAddress (std::string &address) {
        auto is_space = [](char c) {
            return c == ' ' || c == '\t';
        };

        while (!address.empty() && is_space(address.front())) {
            address.erase(address.begin());
        }
        while (!address.empty() && is_space(address.back())) {
            address.pop_back();
        }
    }

    void addAddress (std::string address) {
        trimAddress(address);
        if (!address.empty()) {
            to_list_.insert(std::move(address));
        }
    }

    void addAddressList (postline::json const &value) {
        if (value.is_array()) {
            for (auto const &item : value) {
                CHECK(item.is_string());
                addAddress(item.get<std::string>());
            }
            return;
        }
        if (value.is_string()) {
            std::string_view remaining = value.get_ref<std::string const &>();
            for (;;) {
                std::size_t off = remaining.find(',');
                std::string part;
                if (off == std::string_view::npos) {
                    part.assign(remaining);
                    addAddress(std::move(part));
                    return;
                }
                part.assign(remaining.substr(0, off));
                addAddress(std::move(part));
                remaining.remove_prefix(off + 1);
            }
        }
        CHECK(0, "Unexpected Cc header type: {}", value.dump());
    }

    void rebuildToEntries () {
        to_entries_.assign(to_list_.begin(), to_list_.end());
        std::sort(to_entries_.begin(), to_entries_.end());
        if (to_entries_.empty()) {
            to_entries_.push_back("");
        }
        to_selected_ = std::clamp(
            to_selected_,
            0,
            static_cast<int>(to_entries_.size()) - 1);
    }

    std::string selectedTo () const {
        if (to_entries_.empty()) {
            return "";
        }
        CHECK(to_selected_ >= 0);
        CHECK(static_cast<size_t>(to_selected_) < to_entries_.size());
        return to_entries_[static_cast<size_t>(to_selected_)];
    }

    postline::Message makeMessage (std::string &&to,
                                   std::string &&subject,
                                   std::string &&body) {
        postline::json header{
            {"type", "agent:message"},
            {"From", from_},
            {"To", std::move(to)},
            {"Subject", std::move(subject)},
        };
        return postline::Message(std::move(header), std::move(body));
    }

    bool submit () {
        if (!can_send_) {
            return false;
        }

        std::string to = selectedTo();
        if (to.empty()) {
            return false;
        }

        CHECK(send_callback_);
        std::string subject = std::move(subject_);
        std::string body = std::move(body_);
        subject_.clear();
        body_.clear();
        send_callback_(makeMessage(std::move(to),
                                   std::move(subject),
                                   std::move(body)));
        can_send_ = false;
        return true;
    }

    bool sendExit () {
        if (!can_send_) {
            return false;
        }

        CHECK(send_callback_);
        send_callback_(postline::Message(postline::json{
            {"From", from_},
            {"To", "runtime"},
            {"Subject", "bye"},
        }));
        can_send_ = false;
        return true;
    }

    void process (postline::Message &&msg) {
        postline::json const& header = msg.header();
        if (header.contains("type") && !header["type"].is_null()) {
            if (header.value("type", std::string()) == "agent:bye") {
                can_send_ = false;
                if (exit_loop_) {
                    exit_loop_();
                }
                return;
            }
        }

        std::string to;
        if (header.contains("From") && !header["From"].is_null()) {
            to = header["From"].get<std::string>();
            addAddress(to);
        }
        if (header.contains("Reply-To") && !header["Reply-To"].is_null()) {
            to = header["Reply-To"].get<std::string>();
            addAddress(to);
        }
        if (header.contains("To") && !header["To"].is_null()) {
            from_ = header["To"].get<std::string>();
        }
        if (header.contains("Cc") && !header["Cc"].is_null()) {
            addAddressList(header["Cc"]);
        }

        rebuildToEntries();
        if (!to.empty()) {
            auto it = std::lower_bound(to_entries_.begin(), to_entries_.end(), to);
            if (it != to_entries_.end() && *it == to) {
                to_selected_ = static_cast<int>(it - to_entries_.begin());
            }
        }

        can_send_ = !from_.empty() && !selectedTo().empty();
    }

    Component makeComponent () {
        rebuildToEntries();

        InputOption subject_option;
        subject_option.multiline = false;
        subject_option.transform = [](InputState state) {
            if (state.is_placeholder) {
                state.element |= dim;
            }
            if (!state.focused) {
                state.element |= color(Color::GrayDark);
            }
            return state.element;
        };

        InputOption body_option;
        body_option.multiline = true;
        body_option.transform = [](InputState state) {
            if (state.is_placeholder) {
                state.element |= dim;
            }
            if (!state.focused) {
                state.element |= color(Color::GrayDark);
            }
            return state.element;
        };

        DropdownOption to_option;
        to_option.radiobox.entries = &to_entries_;
        to_option.radiobox.selected = &to_selected_;
        to_option.checkbox.transform = [](EntryState const& state) {
            Element prefix = text(state.state ? "v " : "> ");
            Element label = text(state.label);
            if (state.focused) {
                label |= inverted;
            }
            return hbox({
                std::move(prefix),
                std::move(label),
            });
        };
        to_option.transform =
            [](bool open, Element checkbox, Element radiobox) {
                if (open) {
                    return vbox({
                        std::move(checkbox),
                        std::move(radiobox),
                    });
                }
                return std::move(checkbox);
            };
        to_dropdown_ = Dropdown(std::move(to_option));
        subject_input_ = Input(&subject_, "Subject", subject_option);
        body_input_ = Input(&body_, "Write here...", body_option);
        ButtonOption exit_option;
        exit_option.label = "Exit";
        exit_option.on_click = [this] {
            sendExit();
        };
        exit_option.transform = [this](EntryState const& state) {
            Element element = text(state.label);
            if (!can_send_) {
                element |= dim;
            }
            if (state.focused) {
                element |= inverted;
            }
            return element;
        };
        exit_button_ = Button(std::move(exit_option));

        auto top = Container::Horizontal({
            exit_button_,
            to_dropdown_,
            subject_input_,
        });
        auto layout = Container::Vertical({
            top,
            body_input_,
        });

        auto component = Renderer(layout, [&] {
            Element to_field = hbox({
                text("To: "),
                to_dropdown_->Render() | flex,
            }) | bgcolor(Color::Blue);
            Element subject_field = hbox({
                text("Subject: "),
                subject_input_->Render() | flex,
            }) | bgcolor(Color::Black);
            Element exit_field = hbox({
                exit_button_->Render(),
            }) | bgcolor(Color::DarkRed);

            return vbox({
                       hbox({
                           exit_field,
                           text(" "),
                           to_field | size(WIDTH, EQUAL, 32),
                           text(" "),
                           subject_field | flex,
                       }),
                       body_input_->Render() | flex,
                   }) |
                   flex;
        });

        component = CatchEvent(component, [&](Event event) {
            if (IsSubmitShortcut(event)) {
                return submit();
            }
            if (event == Event::Return && !body_input_->Focused()) {
                return submit();
            }
            return false;
        });

        return component;
    }
public:
    explicit CLI (std::function<void(postline::Message &&)> sendCallback)
        : send_callback_(std::move(sendCallback))
    {
        CHECK(send_callback_);
        rebuildToEntries();
    }

    void recv (postline::Message &&msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (screen_ == nullptr) {
            pending_messages_.push_back(std::move(msg));
            return;
        }

        auto pending = std::make_shared<postline::Message>(std::move(msg));
        screen_->Post([this, pending]() mutable {
            process(std::move(*pending));
        });
        screen_->PostEvent(Event::Custom);
    }

    void run () {
        auto screen = ScreenInteractive::FitComponent();
        auto component = makeComponent();

        std::vector<postline::Message> queued_messages;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            screen_ = &screen;
            exit_loop_ = screen.ExitLoopClosure();
            queued_messages.swap(pending_messages_);
        }

        for (auto &msg : queued_messages) {
            process(std::move(msg));
        }

        body_input_->TakeFocus();
        screen.Loop(component);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            exit_loop_ = Closure();
            screen_ = nullptr;
        }
    }
};

}  // namespace ftxui
