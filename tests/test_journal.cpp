#include <postline/journal.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

postline::Message make_message(const std::string& type, int value) {
    postline::json header{
        {"type", type},
        {"value", value},
    };
    postline::json body{
        {"payload", "message-" + std::to_string(value)},
    };
    return postline::Message(std::move(header), body.dump());
}

void test_append_and_read_single_segment(const fs::path& root) {
    const fs::path segment = root / "single.segment";

    postline::Journal journal(segment.string(), "", [](const postline::Message&) {});

    auto first = make_message("alpha", 1);
    auto second = make_message("beta", 2);

    postline::AccessID first_id = journal.append(first);
    postline::AccessID second_id = journal.append(second);

    expect(first_id != postline::NO_ACCESS_ID, "first access id should be valid");
    expect(second_id != postline::NO_ACCESS_ID, "second access id should be valid");
    expect(first_id != second_id, "appends should return distinct access ids");

    auto first_read = journal.read(first_id);
    auto second_read = journal.read(second_id);

    expect(first_read.header().at("type") == "alpha", "first record header mismatch");
    expect(second_read.header().at("type") == "beta", "second record header mismatch");
}

void test_replay_existing_chain_and_append_new_segment(const fs::path& root) {
    const fs::path first_segment = root / "chain-0001.segment";
    const fs::path second_segment = root / "chain-0002.segment";

    {
        postline::Journal first(first_segment.string(), "", [](const postline::Message&) {});
        first.append(make_message("seed", 10));
        first.append(make_message("seed", 11));
    }

    std::vector<std::string> replayed_types;
    {
        postline::Journal second(
            second_segment.string(),
            first_segment.string(),
            [&](const postline::Message& message) {
                replayed_types.push_back(message.header().at("type").get<std::string>());
            });

        expect(replayed_types.size() == 2, "expected replay from previous segment");
        expect(replayed_types[0] == "seed", "first replayed record mismatch");
        expect(replayed_types[1] == "seed", "second replayed record mismatch");

        postline::AccessID chained_id = second.append(make_message("fresh", 12));
        auto chained_read = second.read(chained_id);
        expect(chained_read.header().at("type") == "fresh", "new chained record mismatch");
    }

    std::vector<std::string> reopened_types;
    postline::Journal reopened(
        "",
        second_segment.string(),
        [&](const postline::Message& message) {
            reopened_types.push_back(message.header().at("type").get<std::string>());
        });

    expect(reopened_types.size() == 3, "expected replay across the full chain");
    expect(reopened_types[0] == "seed", "reopened replay first record mismatch");
    expect(reopened_types[1] == "seed", "reopened replay second record mismatch");
    expect(reopened_types[2] == "fresh", "reopened replay third record mismatch");
}

} // namespace

int main() {
    const fs::path root = "postline-journal-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    try {
        test_append_and_read_single_segment(root);
        test_replay_existing_chain_and_append_new_segment(root);
        fs::remove_all(root);
    } catch (...) {
        //fs::remove_all(root);
        throw;
    }

    std::cout << "journal tests passed\n";
    return 0;
}
