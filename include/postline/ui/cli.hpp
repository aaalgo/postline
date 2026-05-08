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
#include <postline/protocol.h>

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

namespace ftxui {
using postline::check_fail;

static inline Element waiting(int tick)
{
    static const std::array<char const*, 10> frames = {
        "⠋", "⠙", "⠹", "⠸", "⠼",
        "⠴", "⠦", "⠧", "⠇", "⠏",
    };

    static const std::array<Color, 6> colors = {
        Color::Cyan,
        Color::Blue,
        Color::Magenta,
        Color::Red,
        Color::Yellow,
        Color::Green,
    };

    auto frame = frames[tick % frames.size()];
    auto fg = colors[tick % colors.size()];

    return hbox({
        text(frame) | color(fg),
        text(" Waiting for response ..."),
    }) | bold;
}

// CLI displays a compact email composer for the current conversation.
//
// Root layout: status row, To, Subject, then body editor.
// Hitting Enter outside the body editor, or Alt+Enter/F12 anywhere,
// sends the current draft when canSend_ is true.
class CLI {
    std::string from_;
    std::unordered_set<std::string> to_list_;
    std::vector<std::string> to_entries_;
    std::string subject_;
    std::string body_;
    int to_selected_ = 0;
    bool trackTo = true;
    bool can_send_ = false;
    int tick_ = 0;

    std::function<void(postline::Message &&)> send_callback_;
    std::mutex mutex_;
    std::vector<postline::Message> pending_messages_;
    ScreenInteractive* screen_ = nullptr;
    std::binary_semaphore loop_ready_{0};
    Closure exit_loop_;

    Component to_dropdown_;
    Component track_checkbox_;
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
            if (address[0] != '[') {
                to_list_.insert(std::move(address));
            }
        }
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
            {"Subject", "exit"},
        }));
        can_send_ = false;
        return true;
    }

    void process (postline::Message &&msg) {
        std::string const &type = msg.type();
        if (type == postline::protocol::handshake::Bye::type) {
            std::cerr << "BYE RECEIVED" << std::endl;
            can_send_ = false;
            if (exit_loop_) {
                exit_loop_();
            }
            return;
        }

        from_ = msg.to();

        std::string sender = msg.from();
        std::string to = sender;
        addAddress(sender);
        {
            std::string reply_to = msg.get("Reply-To");
            if (!reply_to.empty()) {
                addAddress(reply_to);
                to.swap(reply_to);
            }
        }
        for (std::string const &a: msg.cc()) {
            addAddress(a);
        }

        std::string const &subject = msg.subject();
        if (to == "runtime" && (subject == "Re: spawn" || subject == "Re: list_agents")) {
            postline::json j = postline::json::parse(msg.body());
            for (size_t i = 0; i < j.size(); ++i) {
                auto const &m = j[i];
                if (m.contains("address")) {
                    addAddress(m["address"].get<std::string>());
                }
            }
        }

        rebuildToEntries();
        if (trackTo && !sender.empty()) {
            auto it = std::lower_bound(to_entries_.begin(), to_entries_.end(), sender);
            if (it != to_entries_.end() && *it == sender) {
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
        CheckboxOption track_option;
        track_option.transform = [](EntryState const& state) {
            Element prefix = text(state.state ? "[x] " : "[ ] ");
            Element label = text(state.label);
            if (state.focused) {
                label |= inverted;
            }
            return hbox({
                std::move(prefix),
                std::move(label),
            });
        };
        track_checkbox_ = Checkbox("Track", &trackTo, track_option);
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

        auto layout = Container::Vertical({
            exit_button_,
            to_dropdown_,
            track_checkbox_,
            subject_input_,
            body_input_,
        });

        auto component = Renderer(layout, [&] {
            Element exit_field = hbox({
                exit_button_->Render(),
            }) | bgcolor(Color::DarkRed);
            Element status_field = can_send_
                ? text(" Ready, F12 to send.") | color(Color::Green)
                : waiting(tick_);
            Element to_field = hbox({
                text("To: "),
                to_dropdown_->Render() | flex,
                text(" "),
                track_checkbox_->Render(),
            }) | bgcolor(Color::Blue);
            Element subject_field = hbox({
                text("Subject: "),
                subject_input_->Render() | flex,
            }) | bgcolor(Color::Black);
            
            return vbox({
                       hbox({
                           exit_field,
                           text(" "),
                           status_field | flex,
                       }),
                       to_field,
                       subject_field,
                       body_input_->Render() | frame | flex,
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
        auto screen = ScreenInteractive::Fullscreen();
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
        loop_ready_.release();

        std::atomic<bool> running = true;
        std::thread ticker([&] {
            while (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                tick_++;
                screen.PostEvent(Event::Custom);
            }
        });

        screen.Loop(component);

        running = false;
        ticker.join();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            exit_loop_ = Closure();
            screen_ = nullptr;
        }
    }

    void request_exit()
    {
        Closure exit;
        loop_ready_.acquire();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            exit = exit_loop_;
        }

        if (exit) {
            exit();
        }
    }
};

}  // namespace ftxui
