#include <string>
#include <string_view>
#include <unordered_map>

namespace postline {

class Accounting {
public:
    static constexpr std::string_view PREFIX = "Postline-Cost:";

    std::unordered_map<std::string, double> values;

public:
    void update(Message const& msg)
    {
        json const& h = msg.header();

        for (auto const& [key, value_json] : h.items()) {

            if (!key.starts_with(PREFIX)) {
                continue;
            }

            double value = 0.0;

            try {
                value = std::stod(
                    value_json.get_ref<std::string const&>());
            }
            catch (...) {
                continue;
            }

            std::string accounting_key =
                key.substr(PREFIX.size());

            values[accounting_key] += value;
        }
    }

    json dump() const
    {
        json ret = json::object();

        for (auto const& [key, value] : values) {
            ret[key] = value;
        }

        return ret;
    }
};

} // namespace postline
