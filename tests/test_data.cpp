#include <postline/observer.h>
#include <postline/protocol.h>
#include <postline/service.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, std::string const &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class DataService : public postline::LinearService {
protected:
    void call(postline::Message &&, postline::Response &response) override {
        response.append(postline::protocol::agent::Data::make("first"));
        response.append(postline::protocol::agent::Data::make("second"));
        response.append(postline::Message(postline::json{{"Subject", "done"}}));
    }
};

class TestObserver : public postline::ui::Observer {
public:
    TestObserver() = default;
    using Observer::consume;
    using Observer::process;
};

postline::Message make_incoming() {
    return postline::Message(postline::json{
        {"From", "user"},
        {"To", "worker"},
        {"Thread-ID", "0"},
        {"Message-ID", "42"},
    });
}

postline::Message make_preprocessed_data(postline::Program &program,
                                         postline::AccessID id) {
    postline::Message data = postline::protocol::agent::Data::make("payload");
    data.updateHeader([](postline::json &header) {
        header["From"] = "forged";
        header["Thread-ID"] = "0";
        header[postline::CONTEXT_HEADER_NAME] = "forged";
    });
    program.preprocess(program.user, data, nullptr);
    data.set_access_id(id);
    return data;
}

void test_linear_service_round() {
    DataService service;
    auto messages = service.on_message(make_incoming());

    expect(messages.size() == 3, "DATA round should retain every record");
    for (std::size_t i = 0; i < 2; ++i) {
        expect(messages[i].type() == postline::protocol::agent::Data::type,
               "DATA must precede the routed response");
        expect(messages[i].from() == "worker", "DATA sender was not stamped");
        expect(messages[i].get("Thread-ID") == "0", "DATA thread was not stamped");
        expect(messages[i].to().empty(), "DATA unexpectedly has a destination");
        expect(messages[i].in_reply_to() == postline::NO_ACCESS_ID,
               "DATA unexpectedly has reply routing");
    }
    expect(messages.back().type() != postline::protocol::agent::Data::type,
           "routed response must be final");
    expect(messages.back().to() == "user", "response should return to caller");
    expect(messages.back().in_reply_to() == 42, "response reply id mismatch");
}

void test_program_and_observer_apply() {
    postline::Program program;
    std::size_t user_memory = program.user->memory.size();
    std::size_t zero_memory = program.zero->memory.size();
    std::size_t trace_size = program.threads[0]->trace.size();
    int user_obligations = program.user->obligation_count;
    int zero_obligations = program.zero->obligation_count;

    postline::Message data = make_preprocessed_data(program, 100);
    auto result = program.apply(data);

    expect(result.tag == postline::EntityRef::Tag::NONE, "DATA must not dispatch");
    expect(program.user->memory.size() == user_memory + 1,
           "DATA missing from producer memory");
    expect(program.user->memory.back() == 100, "DATA must be stored as owned memory");
    expect(program.zero->memory.size() == zero_memory, "DATA changed receiver memory");
    expect(program.threads[0]->trace.size() == trace_size, "DATA changed thread trace");
    expect(program.user->obligation_count == user_obligations &&
           program.zero->obligation_count == zero_obligations,
           "DATA changed obligations");
    expect(data.from() == "user", "runtime did not enforce DATA producer");
    expect(data.header().at(postline::CONTEXT_HEADER_NAME).is_object(),
           "runtime did not replace forged DATA context");

    TestObserver observer;
    postline::Message observed = make_preprocessed_data(observer, 101);
    observer.consume(std::move(observed));
    observer.process();
    expect(observer.user->memory.size() == 1, "observer did not replay DATA memory");
    expect(observer.threads[0]->trace.empty(), "observer added DATA to trace");
    expect(observer.getMessage(101)->type() == postline::protocol::agent::Data::type,
           "observer did not cache DATA");
}

} // namespace

int main() {
    test_linear_service_round();
    test_program_and_observer_apply();
    std::cout << "DATA tests passed\n";
    return 0;
}
