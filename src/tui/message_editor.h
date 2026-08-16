#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <postline/common.h>
#include <postline/program.h>

namespace postline { namespace ui {

class MessageEditor {
    using AddressProvider = std::function<void(std::vector<Agent *> *)>;
    using SendCallback = std::function<void(Message&&)>;

    Thread const *thread;
    AddressProvider address_provider;
    SendCallback on_send;

    std::vector<Agent *> address_agents;
    std::vector<std::string> address_labels;
    int address_selected = 0;
    bool clone_checked = false;
    std::string tags_content;
    std::string subject_content;
    std::string body_content;

    ftxui::Component address_choice;
    ftxui::Component send_button;
    ftxui::Component clone_checkbox;
    ftxui::Component tags_editor;
    ftxui::Component subject_editor;
    ftxui::Component body_editor;
    ftxui::Component renderer;

    void syncAddresses();
    void sendMessageTo(Agent *to);

public:
    explicit MessageEditor(Thread const *thread_,
                           AddressProvider address_provider_,
                           SendCallback on_send_);

    bool sendCurrentMessage();
    ftxui::Component component();
};

}}
