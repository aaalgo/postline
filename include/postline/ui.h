#pragma once
#include <future>
#include <fstream>
#include <postline/runtime.h>
#include <spdlog/details/log_msg.h>

namespace postline { namespace ui {

    class UI: public Service {
        struct State {
            bool pending = false;
            bool wants_result = false;
            std::promise<Message> promise;
        };
        std::vector<std::unique_ptr<State>> threads;
        std::mutex mutex;

        void ensure_threads (ThreadID id) {
            CHECK(id >= 0);
            while (id >= threads.size()) {
                threads.push_back(std::make_unique<State>());
            }
        }
    protected:
        Runtime *rt;

        Message syscall(ThreadID thread_id, Message &&msg) {
            CHECK(thread_id >= 0);

            std::future<Message> future;

            {
                std::lock_guard lock(mutex);
                ensure_threads(thread_id);

                auto &thread = *threads[thread_id];

                CHECK(!thread.pending);

                thread.pending = true;
                thread.wants_result = true;

                thread.promise = std::promise<Message>{};
                future = thread.promise.get_future();
            }
            log::info("thread {} set pending", thread_id);
            msg.updateHeader([thread_id](json &h) {
                    h["Thread-ID"] = std::format("{}", thread_id);
                    });
            rt->enqueueUser(std::move(msg));
            return future.get();
        }

        void send(int thread_id, Message &&msg) {
            CHECK(thread_id >= 0);

            {
                std::lock_guard lock(mutex);
                ensure_threads(thread_id);

                auto &thread = *threads[thread_id];

                CHECK(!thread.pending);

                thread.pending = true;
                thread.wants_result = false;

                thread.promise = std::promise<Message>{};
            }
            log::info("thread {} set pending", thread_id);
            msg.updateHeader([thread_id](json &h) {
                    h["Thread-ID"] = std::format("{}", thread_id);
                    });
            rt->enqueueUser(std::move(msg));
        }

        virtual std::vector<Message> on_message(Message &&msg) override {
            // handle syscall results
            // update threads
            //msg.formatEmail(std::cerr);

            int thread_id = msg.thread_id();

            if (!(thread_id >= 0)) {
                std::ofstream xx("xx");
                msg.formatEmail(xx);
            }

            CHECK(thread_id >= 0);

            std::promise<Message> promise;
            bool wants_result = false;

            {
                std::lock_guard lock(mutex);

                CHECK(thread_id < (int)threads.size());
                CHECK(threads[thread_id]);

                auto &thread = *threads[thread_id];

                CHECK(thread.pending);

                wants_result = thread.wants_result;

                if (wants_result) {
                    promise = std::move(thread.promise);
                    thread.promise = std::promise<Message>{};
                }

                thread.pending = false;
                thread.wants_result = false;
                log::info("thread {} clear pending", thread_id);
            }

            if (wants_result) {
                promise.set_value(std::move(msg));
            }

            return {};
        }

    public:

        UI (): rt(nullptr) {
        }

        virtual void setRuntime (Runtime *rt_) {
            rt = rt_;
        }

        virtual ~UI ();

        void initArena (json const &spec);

        virtual std::function<void(Message &&)> consume () {
            return {};
        }

        virtual void appendLog (spdlog::details::log_msg const&) {
        }

        virtual void run () = 0;
    };

    std::unique_ptr<UI> make_ui (std::string const &name);

}}
