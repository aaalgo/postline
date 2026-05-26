#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <postline/runtime.h>
#include <spdlog/details/log_msg_buffer.h>

#include "../ftx_list.hpp"
#include "limits.h"
#include "observer.h"

namespace postline { namespace ui {

struct Tab {
    virtual ~Tab() = default;

    virtual std::string label() const = 0;
    virtual ftxui::Component component() = 0;
    virtual ThreadID threadId() const;
};

struct GlobalData {
    Runtime *runtime;
    ftxui::ListData<spdlog::details::log_msg_buffer> log_entries;
    ftxui::ListData<std::string> threads;

    std::function<void(ThreadID)> onOpenThread;

    GlobalData();
};

class GlobalTab : public Tab {
    GlobalData *data;
    int thread_selected = 0;
    bool thread_follow_tail = false;
    int log_selected = -1;
    bool log_follow_tail = true;

    ftxui::Component thread_list;
    ftxui::Component log_list;
    ftxui::Component left_renderer;
    ftxui::Component right_renderer;
    ftxui::Component root;
    ftxui::Component renderer;

public:
    explicit GlobalTab(GlobalData *d);

    std::string label() const override;
    ftxui::Component component() override;
};

class StatTab : public Tab {
    struct StatSnapshot {
        std::vector<std::string> process_lines;
        std::vector<std::string> system_lines;
        std::vector<std::string> pstree_lines;
    };

    StatSnapshot snapshot;
    std::chrono::steady_clock::time_point last_refresh;
    int tree_scroll = 0;

    ftxui::Component renderer;

    void refresh();
    ftxui::Element renderProcessStats() const;
    ftxui::Element renderSystemStats() const;
    ftxui::Element renderProcessTree() const;

public:
    StatTab();

    std::string label() const override;
    ftxui::Component component() override;
};

class MessageReader {
    Message const **message;

    int message_scroll = 0;
    int message_line_count = 0;

    ftxui::Component renderer;

    ftxui::Element renderMessage();
    bool onMessageViewerEvent(ftxui::Event event);

public:
    explicit MessageReader(Message const **message_);

    void resetScroll();
    ftxui::Component component();
};

class MessageEditor {
    using SendCallback = std::function<void(std::string,
                                            std::string,
                                            std::string)>;

    Thread const *thread;
    SendCallback on_send;

    std::vector<std::string> addresses = {
        "runtime",
        "echo",
        "ai1",
        "ai2",
        "ai3",
        "shell",
        "mcp",
        "memory",
        "login",
        "benchmark",
    };
    int address_selected = 0;
    std::string subject_content;
    std::string body_content;

    ftxui::Component address_choice;
    ftxui::Component send_button;
    ftxui::Component subject_editor;
    ftxui::Component body_editor;
    ftxui::Component renderer;

public:
    explicit MessageEditor(Thread const *thread_,
                           SendCallback on_send_);

    ftxui::Component component();
};

class MessageListDataRef: public ftxui::ListDataRef {
  Observer *observer;
  std::vector<AccessID> *data;
public:
  MessageListDataRef (Observer *observer_, std::vector<AccessID> *data_)
      :observer(observer_),
      data(data_)
  {
  }

  virtual int firstValid() const {
      return 0;
  }
  virtual int end() const {
      return data->size();
  }
  virtual ftxui::Element Render(int index) const {
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
      return ftxui::text(std::format("{} -> {}: {}", from->name, to->name, msg.subject()));
      }
      catch (...) {
          return ftxui::text("no context");
      }
  }
};


class ThreadTab : public Tab {
public:
    using ReadMessageCallback = std::function<Message const *(AccessID)>;
    using SendCallback = std::function<void(ThreadID, Message&&)>;

private:
    Thread *data;
    MessageListDataRef trace;
    ReadMessageCallback read_message;
    SendCallback on_send;

    Message const *current_message;
    AccessID current_message_id = NO_ACCESS_ID;
    MessageReader message_reader;
    MessageEditor message_editor;

    int nav_mode = 0;
    int nav_selected = 0;
    int trace_selected = 0;
    bool trace_follow_tail = true;

    std::vector<std::string> nav_modes = {
        "tree", "stack", "members",
    };

    std::vector<std::string> tree_entries = {
        "/",
        "/build",
        "/build/compiler",
        "/build/test",
        "/ui",
    };

    std::vector<std::string> stack_entries = {
        "user",
        "planner",
        "writer",
        "compiler",
    };

    std::vector<std::string> member_entries = {
        "@user",
        "@runtime",
        "@planner",
        "@writer",
        "@compiler",
        "domain:build",
        "domain:ui",
    };

    ftxui::Component nav_mode_menu;
    ftxui::Component tree_list;
    ftxui::Component stack_list;
    ftxui::Component member_list;
    ftxui::Component nav_content;
    ftxui::Component trace_list;

    ftxui::Component left_renderer;
    ftxui::Component middle_renderer;
    ftxui::Component right_renderer;
    ftxui::Component root;
    ftxui::Component renderer;

    void reloadCurrentMessage();

public:
    explicit ThreadTab(Observer *observer, Thread *data_,
                       ReadMessageCallback read_message_,
                       SendCallback on_send_);

    std::string label() const override;
    ftxui::Component component() override;
    ThreadID threadId() const override;
};

}}
