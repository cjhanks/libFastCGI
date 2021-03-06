#include "WsgiServer.hpp"

#include <csignal>

#include "SimpleCGI/common/Logging.hpp"
#include "WsgiReader.hpp"
#include "WsgiError.hpp"
#include "WsgiStartResponse.hpp"
#include "PythonHelper.hpp"


namespace fcgi {
using fcgi::bits::Decref;
using fcgi::bits::AutoGIL;


WsgiApplication::WsgiApplication(Config config)
  : config(config)
  , module(nullptr)
  , application(nullptr)
{
  LOG(INFO) << "Initialize";
  // 1.  Initialize the python library
  Py_Initialize();
  PyEval_InitThreads();
}

WsgiApplication::~WsgiApplication()
{
  AutoGIL gil;

  if (application)
    Py_DECREF(application);

  if (module)
    Py_DECREF(module);

  Py_Finalize();
}

void
WsgiApplication::Initialize()
{
  AutoGIL gil;

  // 2.  Load the provided module
  PyObject* moduleName = PyUnicode_FromString(config.module.c_str());
  if (moduleName == nullptr) {
    PyErr_Print();
    throw std::runtime_error("Failed to interpret module name");
  }

  module = PyImport_Import(moduleName);
  Py_DECREF(moduleName);
  if (module == nullptr) {
    PyErr_Print();
    LOG(ERROR) << "Failed to load: " << config.module;
    throw std::runtime_error("Failed to load module");
  }

  // 3.  Load the application
  application = PyObject_GetAttrString(module, config.app.c_str());
  if (application == nullptr) {
    PyErr_Print();
    throw std::runtime_error("Failed to load application");
  }

  // 4.
  WsgiHandler::WsgiVersionTuple = PyTuple_Pack(
      2,
      PyLong_FromLong(1),
      PyLong_FromLong(0)
  );
}

bool
WsgiApplication::Serve(fcgi::HttpRequest& req, fcgi::HttpResponse& res)
{
  return WsgiHandler(req, res)(application);
}

////////////////////////////////////////////////////////////////////////////////
PyObject*
WsgiHandler::WsgiVersionTuple = nullptr;

WsgiHandler::WsgiHandler(fcgi::HttpRequest& req, fcgi::HttpResponse& res)
  : req(req)
  , res(res)
  , environment(nullptr)
  , callback(New(&res))
{
  AutoGIL gil;
  environment = PyDict_New();
}

WsgiHandler::~WsgiHandler()
{
  AutoGIL gil;

  if (environment)
    Py_DECREF(environment);

  if (callback)
    Py_DECREF(callback);
}

bool
WsgiHandler::operator()(PyObject* wsgiFunctor)
{
  PopulateEnvironmentCGI();
  PopulateEnvironmentWSGI();

  assert(nullptr != wsgiFunctor);
  assert(nullptr != environment);
  assert(nullptr != callback);
  //LOG::CHECK(nullptr != wsgiFunctor);
  //LOG::CHECK(nullptr != environment);
  //LOG::CHECK(nullptr != callback);

  Decref results =
    PyObject_CallFunctionObjArgs(wsgiFunctor, environment, callback, nullptr);
  if (results == nullptr)
    return false;

  Decref iterator = PyObject_GetIter(results);
  if (!PyIter_Check(iterator))
    return false;

  while (PyObject* elem = PyIter_Next(iterator)) {
    if (!PyBytes_Check(elem)) {
      LOG(WARNING) << "UNKNOWN TYPE";
    }

    fcgi::Ostream ostr = res.ToStream();
    Py_BEGIN_ALLOW_THREADS
    // FIXME ?
    ostr.write(PyBytes_AsString(elem), PyBytes_GET_SIZE(elem));
    Py_END_ALLOW_THREADS

    Py_DECREF(elem);
  }

  return true;
}

namespace {
void
AddToDict(PyObject* dict, PyObject* k, PyObject* v)
{
    PyDict_SetItem(dict, k, v);
    Py_DECREF(k);

    if (v == Py_False || v == Py_True)
      return;

    Py_DECREF(v);
}
} // ns

void
WsgiHandler::PopulateEnvironmentCGI()
{
  AutoGIL gil;
  for (const auto& pair: req.Headers()) {
    AddToDict(
      environment,
      PyUnicode_FromString(pair.first.c_str()),
      PyUnicode_FromString(pair.second.c_str())
    );
  }

  AddToDict(
      environment,
      PyUnicode_FromString("PATH_INFO"),
      PyUnicode_FromString(req.GetHeader("DOCUMENT_URI").c_str())
  );
}

void
WsgiHandler::PopulateEnvironmentWSGI()
{
  AutoGIL gil;
  PyObject* name = PyUnicode_FromString("wsgi.version");
  PyDict_SetItem(
      environment,
      name,
      WsgiVersionTuple
  );
  Py_DECREF(name);

  AddToDict(
      environment,
      PyUnicode_FromString("wsgi.url_scheme"),
      PyUnicode_FromString(req.GetHeader("REQUEST_SCHEME").c_str())
  );

  AddToDict(
      environment,
      PyUnicode_FromString("wsgi.multithread"),
      Py_False
  );

  AddToDict(
      environment,
      PyUnicode_FromString("wsgi.multiprocess"),
      Py_False
  );

  AddToDict(
      environment,
      PyUnicode_FromString("wsgi.input"),
      New(&req)
  );

#if 0
  PyDict_SetItem(
      environment,
      PyUnicode_FromString("wsgi.error"),
      New(&res)
  );
#endif
}
} // ns fcgi
