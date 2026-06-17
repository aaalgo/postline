#pragma once

#include <format>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <postline/observer.h>

#include "ftx_list.hpp"

namespace postline { namespace ui {

class MessageListDataRef: public ftxui::ListDataRef {
    Observer *observer;
    std::vector<AccessID> *data;

public:
    MessageListDataRef(Observer *observer_, std::vector<AccessID> *data_)
        : observer(observer_),
          data(data_) {
    }

    int firstValid() const override {
        return 0;
    }

    int end() const override {
        return data->size();
    }

    ftxui::Element Render(int index) const override {
        try {
            Message const &msg = *observer->getMessage(data->at(index));
            if (!msg.header().contains(CONTEXT_HEADER_NAME)) {
                return ftxui::text("cannot load message");
            }
            json const &ctx = msg.header().at(CONTEXT_HEADER_NAME);
            AgentID from_agent_id = ctx.at("from_agent_id").get<AgentID>();
            AgentID to_agent_id = ctx.at("to_agent_id").get<AgentID>();
            Agent *from = observer->agents[from_agent_id].get();
            Agent *to = observer->agents[to_agent_id].get();
            return ftxui::text(std::format("{} -> {}: {}",
                                           from->name,
                                           to->name,
                                           msg.subject()));
        }
        catch (...) {
            return ftxui::text("no context");
        }
    }
};

}}
