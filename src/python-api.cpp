#include <sstream>
#include <pybind11/pybind11.h>
#include <postline/common.h>
#include <postline/driver.h>

namespace py = pybind11;

namespace postline {

json py2json (py::object obj)
{
    CHECK(py::isinstance<py::dict>(obj));

    json ret = json::object();
    py::dict dict = obj.cast<py::dict>();
    for (auto const &item : dict) {
        CHECK(py::isinstance<py::str>(item.first));
        std::string key = py::cast<std::string>(item.first);

        if (py::isinstance<py::str>(item.second)) {
            ret[key] = py::cast<std::string>(item.second);
        }
        else if (py::isinstance<py::int_>(item.second)) {
            ret[key] = py::cast<int64_t>(item.second);
        }
        else {
            CHECK(0);
        }
    }
    return ret;
}

class PyMessage: public Message {
public:
    PyMessage () {}

    PyMessage(Message&& msg): Message(std::move(msg)) {}

    PyMessage(py::object header, std::string body_raw = "")
        : Message(py2json(header), std::move(body_raw))
    {}

    py::object py_get (std::string const &key) const {
        auto const &h = header();
        auto it = h.find(key);
        if (it == h.end() || it->is_null()) {
            return py::none();
        }
        if (it->is_string()) {
            return py::str(it->get<std::string>());
        }
        if (it->is_number_integer()) {
            return py::int_(it->get<int64_t>());
        }
        CHECK(0);
    }

    void py_set (std::string const &key, py::object value) {
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

    void updateResponseFields (PyMessage const &last) {
        updateHeader([&last, this](json &header) {
            if (last.from() == to()) {
                header["In-Reply-To"] = last.header()["Message-ID"];
            }
            else {
                header["In-Response-To"] = last.header()["Message-ID"];
            }
	    header["Thread-ID"] = last.header()["Thread-ID"];
        });
    }

    static PyMessage read (int fd) {
        return PyMessage(Message::read(fd));
    }

    static PyMessage parse (std::string const &v) {
        return PyMessage(Message::parseEmail(v));
    }


    size_t write (int fd) const {
    	return Message::write(fd);
    }

    std::string format (bool compact) {
        std::ostringstream ss;
        formatEmail(ss, compact);
        return ss.str();
    }
};

class ServerLogic {
public:
    // handles:
    //      Thread-ID
    //      In-Reply-To
    //      In-Response-To
    //
    // Currently only supports responsive agent and should be used
    // in Python servers.  It should not be used by CLI/FTXCLI
    //
    void afterReceive (PyMessage const &msg) {
    }

    void beforeSend (PyMessage &msg) {
    }
};

} // namespace postline

PYBIND11_MODULE(_postline, module) {
    module.doc() = "Postline Python extension module";

    py::class_<postline::PyMessage>(module, "Message")
        .def(py::init<>())
        .def(py::init<py::object, std::string>(), py::arg("header"), py::arg("body") = "")
        .def_static("read", &postline::PyMessage::read, py::arg("fd"))
        .def_static("parse", &postline::PyMessage::parse, py::arg("value"))
        .def("get", &postline::PyMessage::py_get, py::arg("key"))
        .def("set", &postline::PyMessage::py_set, py::arg("key"), py::arg("value"))
        .def("write", &postline::PyMessage::write, py::arg("fd"))
        .def("format", &postline::PyMessage::format, py::arg("compact") = false)
        .def("updateResponseFields", &postline::PyMessage::updateResponseFields);
    py::class_<postline::ServerLogic>(module, "ServerLogic")
        .def(py::init<>())
        .def("afterReceive", &postline::ServerLogic::afterReceive, py::arg("msg"))
        .def("beforeSend", &postline::ServerLogic::beforeSend, py::arg("msg"));
}
