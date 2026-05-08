#include <sstream>
#include <pybind11/pybind11.h>
#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>

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

class PyService: public Service {
    Server::Config config;
    Server server;
public:
    PyService (): server(config) {
    }

    std::string get_address () const {
        return address;
    }

    virtual void on_memory (Message && msg) {
        py::gil_scoped_acquire gil;
        py::cast(this, py::return_value_policy::reference)
        .attr("on_memory")(std::make_unique<Message>(std::move(msg)));
    }

    virtual void init (Response &resp) override {
        py::gil_scoped_acquire gil;
        /*
        py::object py_resp = py::cast(&resp, py::return_value_policy::reference);
        //py::cast(this).attr("on_init")(py_resp);
        py::cast(this, py::return_value_policy::reference).attr("on_init")(py_resp);
        */
        py::object self = py::cast(
            this,
            py::return_value_policy::reference
        );
        py::object py_resp = py::cast(
            &resp,
            py::return_value_policy::reference
        );
        self.attr("on_init")(py_resp);
    }

    virtual void call (Message &&msg, Response &resp) override {
        py::gil_scoped_acquire gil;
        py::object self = py::cast(
            this,
            py::return_value_policy::reference
        );
        py::object py_resp = py::cast(
            &resp,
            py::return_value_policy::reference
        );
        self.attr("on_call")(std::make_unique<Message>(std::move(msg)), py_resp);
    }

    void run () {
        server.run(this);
    }
};

} // namespace postline

PYBIND11_MODULE(_postline, module) {
    using namespace postline;
    module.doc() = "Postline Python extension module";

    py::class_<Message>(module, "Message")
        .def(py::init<>())
        .def(py::init(
                [](py::object header, std::string body_raw) {
                    return std::make_unique<Message>(
                        py2json(header),
                        std::move(body_raw)
                    );
                }),
                py::arg("header"), py::arg("body") = "")
        .def_static("read", 
                [](int fd) {
                    Message msg = Message::read(fd);
                    return std::make_unique<Message>(std::move(msg));
                },
                py::arg("fd"))
        .def_static("parse",
                [](std::string const &text)
                {
                    Message msg = Message::parseEmail(text);
                    return std::make_unique<Message>(std::move(msg));
                },
                py::arg("value"))
        .def("get", &Message::get, py::arg("key"))
        .def("write", &Message::write, py::arg("fd"))
        .def("format", 
                [](Message &msg, bool compact) {
                    std::ostringstream ss;
                    msg.formatEmail(ss, compact);
                    return ss.str();
                }, py::arg("compact") = false)
        ;
    py::class_<PyService>(module, "Service")
        .def(py::init<>())
        .def("run", &PyService::run)
        .def("address", &PyService::get_address)
        ;
    py::class_<Response>(module, "Response")
        .def("append", 
                [](Response& resp, Message &msg) {
                    resp.append(std::move(msg));
                }, py::arg("msg"))
        ;
}
