#include <sstream>
#include <pybind11/pybind11.h>
#include <postline/common.h>

namespace py = pybind11;

namespace postline {

class PyMessage: public Message {
public:
    PyMessage () {}

    PyMessage(Message&& msg): Message(std::move(msg)) {}

    py::object get (std::string const &key) const {
        auto const &h = header();
        auto it = h.find(key);
        if (it == h.end() || it->is_null()) {
            return py::none();
        }
        return py::str(it->get<std::string>());
    }

    void set (std::string const &key, py::object value) {
        if (value.is_none()) {
            updateHeader([&key](json &h) {
                if (h.contains(key)) {
                    h.erase(key);
                }
            });
        } else if (py::isinstance<py::str>(value)) {
            std::string v = value.cast<std::string>();
            updateHeader([&key, &v](json &h) {
                h[key] = v;
            });
        } else if (py::isinstance<py::int_>(value)) {
            int v = value.cast<int>();
            updateHeader([&key, &v](json &h) {
                h[key] = v;
            });
        } else {
            CHECK(0);
        }
    }

    static PyMessage read (int fd) {
        return PyMessage(Message::read(fd));
    }

    static PyMessage parse (std::string const &v) {
        return PyMessage(Message::parseEmail(v));
    }

    std::string format (bool compact) {
        std::ostringstream ss;
        formatEmail(ss, compact);
        return ss.str();
    }
};

} // namespace postline

PYBIND11_MODULE(_postline, module) {
    module.doc() = "Postline Python extension module";

    py::class_<postline::PyMessage>(module, "Message")
        .def(py::init<>())
        .def_static("read", &postline::PyMessage::read, py::arg("fd"))
        .def_static("parse", &postline::PyMessage::parse, py::arg("value"))
        .def("get", &postline::PyMessage::get)
        .def("set", &postline::PyMessage::set)
        .def("write", &postline::Message::write, py::arg("fd"))
        .def("format", &postline::PyMessage::format, py::arg("compact") = false);
}
